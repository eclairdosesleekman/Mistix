# Mistix Phase 15 — Freestanding runtime diagnostics

## Objective

Phase 14 added reproducible builds but left range and integer-overflow checks
disabled because the freestanding kernel and userland had no runtime-error
entry points. Phase 15 closes that gap without linking the hosted Free Pascal
RTL or changing the syscall ABI.

## Build profiles

- Release keeps `{$R-}` and `{$Q-}` for kernel and userland programs.
- Debug uses FPC `-Cr -Co` and defines `MISTIX_DEBUG_CHECKS`. Kernel and the
  shared userland include conditionally select `{$R+}` and `{$Q+}`.
- Both profiles retain `-O-`; debug symbols remain `-g` (not `-gl`).
  The LineInfo RTL unit is not pulled into the freestanding image.

## Runtime entry points

Mistix exports three no-argument compatibility/runtime entry points in every
linked image:

| Symbol | Runtime error | Explicit hook status |
| --- | --- | ---: |
| `FPC_DIVBYZERO` | Division by zero fallback | 200 |
| `fpc_rangeerror` | Range check failure | 201 |
| `FPC_OVERFLOW` | Integer arithmetic overflow | 215 |

The selected x86-64 FPC compiler emits calls to the exact ELF symbols `fpc_rangeerror` and `FPC_OVERFLOW`.
Its signed integer division uses the native `IDIV` instruction instead of an
undefined `FPC_DIVBYZERO` helper; a zero divisor therefore raises CPU exception
vector 0 (`#DE`). Mistix keeps the `FPC_DIVBYZERO` entry point available as a
compatibility fallback, while the kernel trap dispatcher isolates native
user-mode `#DE` faults as process exits. The older uppercase `FPC_RANGEERROR`
spelling remains exported as a compatibility alias. The freestanding implementation does
not call `FPC_HANDLEERROR`, `Halt`, `SysUtils`, or any other hosted RTL routine.

### Kernel

The kernel handlers disable interrupts, write a short fault-specific message
to COM1 using bounded UART polling, and halt forever. They do not allocate
memory, format a backtrace, schedule another process, or continue after a
kernel invariant has failed.

### Userland

Each userland image includes the same three no-argument compiler hooks via
`mistixlib.inc`. A handler issues syscall 2 (exit) with the matching runtime
error number in `RDI`, then executes `UD2` as a defensive unreachable
instruction if the syscall unexpectedly returns. Only the faulting process
is terminated; the kernel remains in control and can reap its exit status.

## Validation

- `make phase15-check` verifies debug/release flag selection, conditional
  Pascal directives, compiler hook symbols, and freestanding termination
  paths.
- `make test` runs all source-contract checks and Python tooling tests without
  requiring FPC or QEMU.
- CI compiles Release and checked Debug profiles, then runs the headless QEMU
  smoke test against both images. The Debug smoke exercises the normal boot,
  process setup, service startup, and init path with checks enabled.

A passing smoke test only demonstrates that the exercised boot path did not
hit a checked runtime error. It is not fault-injection coverage of every
handler, proof of memory safety, or hardware qualification.

## Explicit limits

Phase 15 enables range, integer-overflow, and divide-by-zero diagnostics. It
does not enable stack checks (`{$S+}`), I/O checking (`{$I+}`), floating
point exception handling, language exceptions, heap tracing, or source-line
backtraces. These require separate freestanding support and validation.
