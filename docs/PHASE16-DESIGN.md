# Mistix Phase 16 — FPC runtime-check ABI verification

## Objective

Phase 15 added freestanding compiler-runtime entry points for checked code.
Phase 16 verifies the actual code-generation contract of the selected Free
Pascal compiler and x86-64 target when range and overflow checks are enabled.

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
and signed integer division with a runtime-supplied divisor. The generated
object must reference the range/overflow helpers and retain a native signed
`IDIV` instruction for the division operation.

## Validation target

`make fpc-runtime-check` runs `tools/check_fpc_runtime.py`. The script:

1. Compiles the probe in a temporary directory.
2. Uses GNU `nm -u` to require undefined references to:
   - `fpc_rangeerror` — Pascal range checking;
   - `FPC_OVERFLOW` — the isolated ABI probe's Pascal integer-overflow helper.

Debug userland links can also request the lowercase spelling `fpc_overflow`;
Phase 17 exports both symbol forms so differently compiled units resolve
against the same freestanding handler.
3. Uses GNU `objdump -d` to require a signed `IDIV` instruction.

The selected x86-64 FPC compiler does not emit an undefined
`FPC_DIVBYZERO` symbol for integer division. A zero divisor reaches the CPU's
divide-error exception (`#DE`, vector 0). Mistix still exports
`FPC_DIVBYZERO` as a compatibility entry point, but the actual x86-64 path is
validated by disassembly and guest-side exception testing in Phase 17.

The test reports the actual undefined-symbol list or disassembly when an
expectation is missing. Temporary compiler outputs are discarded after each
run.

`make phase16-check` is host-only and confirms that the compile probe and ABI
validation target remain wired into the repository. `make test` stays usable
on hosts without FPC; the dedicated compiler test is run explicitly by CI,
where FPC and GNU binutils are installed.

## CI order

The workflow runs Python/static checks first, then the compiler symbol and
instruction probe, then Release and Debug full builds, followed by QEMU smoke
tests of both profiles. This catches mismatches between compiler-generated
helpers, native divide instructions, and Mistix's freestanding handlers.

## Scope and limits

- The object check proves range/overflow helper references and native signed
  division code generation; it does not execute handlers.
- Whole-image linking continues to verify that Mistix supplies definitions
  for unresolved compiler/runtime symbols.
- QEMU smoke verifies the normal boot path and, in Debug, the targeted
  userland fault-isolation cases; it is not a full stress test.
- The object-symbol check does not execute those handlers. At the Phase 16
  baseline, guest-side fault injection was still outstanding; Phase 17 adds
  userland range, overflow, and divide-by-zero process-isolation tests. See
  [docs/PHASE17-DESIGN.md](PHASE17-DESIGN.md).

## Acceptance criteria

1. `make phase16-check` passes without FPC or QEMU.
2. `make fpc-runtime-check` finds `fpc_rangeerror` and `FPC_OVERFLOW`
   and confirms native signed `IDIV` code generation.
3. The full Release and Debug kernel/userland links remain free of unexpected
   undefined symbols.
4. CI reaches and passes both profile builds and QEMU smoke tests.
