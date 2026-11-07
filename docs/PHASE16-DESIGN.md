# Mistix Phase 16 — FPC runtime-check ABI verification

## Objective

Phase 15 added freestanding implementations of the compiler hooks for range
errors, integer overflow, and division by zero. Phase 16 verifies that the
selected Free Pascal compiler and x86-64 target emit references to those exact
symbols when the corresponding checks are enabled.

This is a compile-time ABI test. It does not alter the kernel syscall ABI and
does not execute guest-only instructions on the host.

## Probe

`tools/tests/fpc_runtime_probe.pas` is a small Pascal unit compiled with:

- target `linux/x86_64`;
- `-Cn` (compile without linking);
- `-O-` (keep the probe operations visible);
- `-Cr -Co` (range and integer-overflow checks);
- `MISTIX_DEBUG_CHECKS` (the same conditional used by Mistix Debug images).

Its three procedures contain dynamic array indexing, signed integer addition,
and integer division with a runtime-supplied divisor. These expressions cannot
be folded to constants by the source, so the generated object must carry
compiler-runtime references.

## Validation target

`make fpc-runtime-check` runs `tools/check_fpc_runtime.py`. The script
compiles the probe in a temporary directory, invokes GNU `nm -u` on the
object, normalizes symbol spelling, and requires all of:

- `FPC_RANGEERROR` — Pascal range checking;
- `FPC_OVERFLOW` — Pascal integer-overflow checking;
- `FPC_DIVBYZERO` — integer division-by-zero checking.

The test reports the actual undefined-symbol list when an expected hook is
missing. Temporary compiler outputs are discarded after each run.

`make phase16-check` is host-only and confirms that the compile probe and
ABI-validation target remain wired into the repository. `make test` stays
usable on hosts without FPC; the dedicated compiler test is run explicitly
by CI, where the Free Pascal toolchain and binutils are installed.

## CI order

The workflow runs Python/static checks first, then the runtime-symbol probe,
then Release and Debug full builds, followed by QEMU smoke tests of both
profiles. This catches a mismatch between compiler-generated helper calls and
Mistix's freestanding definitions before spending time on full image builds.

## Scope and limits

- The object-symbol check proves that the compiler emits the expected ABI
  references; it does not execute those handlers.
- Whole-image linking continues to verify that Mistix supplies definitions
  for all unresolved compiler/runtime symbols.
- QEMU smoke verifies the existing normal boot path, not deliberate range,
  overflow, or divide-by-zero fault injection.
- A future guest-side fault-injection phase should verify the kernel halt
  diagnostics and that a failing user process can be reaped with the expected
  status code.

## Acceptance criteria

1. `make phase16-check` passes without FPC or QEMU.
2. `make fpc-runtime-check` compiles the probe and finds all three expected
   undefined runtime symbols.
3. The full Release and Debug kernel/userland links remain free of unexpected
   undefined symbols.
4. CI reaches and passes both profile builds and QEMU smoke tests.
