# Mistix Phase 17 — Runtime fault isolation

## Objective

Phase 16 established that the x86-64 Free Pascal compiler emits the three
runtime-check symbols used by Mistix. Its CI run exposed an integration gap:
the shared userland include did not actually define those hooks. Phase 17
implements the userland entry points and validates their effect on process
lifecycle under QEMU.

This phase is deliberately limited to userland faults. Kernel-mode range,
overflow, and divide-by-zero failures continue to use the kernel's fatal COM1
diagnostics and halt path.

## Userland runtime hooks

`userland/common/mistixlib.inc` provides all three compiler ABI symbols:

| Symbol | Runtime fault | Child process exit status |
| --- | --- | ---: |
| `fpc_rangeerror` | range-check violation | 201 |
| `FPC_OVERFLOW` | signed integer overflow | 215 |
| x86 `IDIV` / `#DE` | integer division by zero | `0x8000000000000000` |

The shared library also exports `FPC_DIVBYZERO` as a compatibility fallback
(status 200), and retains uppercase `FPC_RANGEERROR` as a compatibility
alias. The selected x86-64 FPC compiler emits native signed
`IDIV`, not a call to that helper. A zero divisor therefore enters the
kernel's user-mode exception path (vector 0), which tags the process exit
status with the high exception bit. The kernel records that status, marks
only the faulting process as a zombie, wakes its waiting parent, and reaps
the child after the status has been copied out. No hosted RTL is linked.

## Guest-side probe

`userland/tests/mistixruntimefault.pas` is built as a freestanding user ELF and
installed at `/bin/runtimefault` in InitFS. It accepts one controlled mode
from its process command line:

- `range`: indexes a four-element array using a runtime-derived out-of-range
  value;
- `overflow`: adds a positive runtime-derived value to `High(LongInt)`;
- `divide`: forms a zero divisor from runtime-supplied command-line bytes and
  performs integer division.

The Debug profile enables `{$R+}` and `{$Q+}`; reaching the normal return path
is treated as a test failure (status 99). Invalid probe arguments exit with
status 2.

## Process-isolation test

When `MISTIX_DEBUG_CHECKS` is defined, PID 1 runs three sequential cases before
launching the interactive shell. For each case it:

1. spawns `/bin/runtimefault` with the relevant mode;
2. blocks in `SysWaitPID` until that exact child terminates;
3. verifies the returned PID and status (201, 215, or the vector-0 exception tag `0x8000000000000000`);
4. proceeds to the next case only after successful reaping.

A failure emits a serial `[FAIL]` marker and exits init nonzero. After all
three cases, init emits the Phase 17 pass marker. The Makefile's Debug
`smoke` target requires that marker, so a normal boot alone is insufficient
to pass the new test. Release does not execute deliberate faults.

## Validation

- `make phase17-check` validates the hook definitions, test-program wiring,
  expected status mappings, and Debug smoke assertion without requiring FPC
  or QEMU.
- `make test` runs the new static check alongside previous source contracts.
- CI's existing `make debug` build compiles and links the test binary into
  InitFS.
- `make smoke BUILD_TYPE=debug` executes the three fault cases under QEMU and
  checks the success marker.
- Release still runs the existing normal boot smoke path without intentionally
  triggering checked runtime errors.

## Scope and limits

- These cases validate the userland compiler hooks and child process exit /
  wait / reap path. They do not exercise the kernel's fatal runtime handlers.
- The divide-by-zero case validates native `IDIV` / CPU `#DE` delivery and
  user-process termination; it does not call the compatibility-only
  `FPC_DIVBYZERO` wrapper on the selected compiler.
- The tests are deterministic probes, not fuzzing or exhaustive arithmetic
  coverage.
- A passing QEMU smoke test is not a hardware qualification or a general
  memory-safety proof.
- The runtime hooks terminate the faulting process; they do not provide
  exception unwinding or recoverable Pascal exceptions.

## Acceptance criteria

1. The three public FPC userland runtime symbols are defined in the common
   freestanding library.
2. Each Debug child fault exits with its matching status and is reaped by PID 1.
3. The Debug QEMU smoke test requires the Phase 17 pass marker.
4. Release and Debug userland/kernel images remain free of unexpected
   unresolved symbols.
