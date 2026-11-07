#!/usr/bin/env python3
"""Static checks for Mistix Phase 16 compiler-runtime ABI validation."""
from __future__ import annotations
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def require(path: str, tokens: tuple[str, ...]) -> str:
    target = ROOT / path
    if not target.is_file():
        raise SystemExit(f"missing file: {path}")
    source = target.read_text(encoding="utf-8")
    missing = [token for token in tokens if token not in source]
    if missing:
        raise SystemExit(f"{path}: missing {', '.join(missing)}")
    return source

def main() -> int:
    makefile = require("Makefile", (
        "tools/check_phase16.py", "phase16-check:",
        "fpc-runtime-check:", "tools/check_fpc_runtime.py",
    ))
    if makefile.count("tools/check_phase16.py") != 2:
        raise SystemExit("Phase 16 source check must run from check-source and phase16-check.")

    require("docs/PHASE14-DESIGN.md", (
        "does not enable Pascal range or overflow checks",
        "Phase 15 completes",
    ))
    require("tools/tests/fpc_runtime_probe.pas", (
        "procedure ProbeRange(Index: LongInt)",
        "ProbeTable[Index] := Byte(Index)",
        "procedure ProbeOverflow(Left, Right: LongInt)",
        "ProbeArithmeticResult := Left + Right",
        "procedure ProbeDivideByZero(Numerator, Denominator: LongInt)",
        "Numerator div Denominator",
        "{$R+}", "{$Q+}",
    ))
    require("tools/check_fpc_runtime.py", (
        "-Tlinux", "-Px86_64", "-Cn", "-O-", "-Cr", "-Co",
        "fpc_rangeerror", "FPC_OVERFLOW", "OBJDUMP",
        "objdump_command + [", "SIGNED_DIVISION", "TemporaryDirectory",
    ))
    require("docs/PHASE16-DESIGN.md", (
        "FPC_RANGEERROR", "FPC_OVERFLOW", "IDIV", "#DE",
        "make fpc-runtime-check", "compile-time ABI test",
        "does not execute those handlers",
    ))
    require(".github/workflows/ci.yml", (
        "Verify checked FPC runtime ABI",
        "make fpc-runtime-check",
    ))
    require("README.md", (
        "Phase 16 — FPC runtime-check ABI verification",
        "make fpc-runtime-check", "make phase16-check",
        "docs/PHASE16-DESIGN.md",
    ))
    print("[ OK ] Phase 14 historical checker contract remains valid.")
    print("[ OK ] FPC probe covers range, overflow, and native signed division.")
    print("[ OK ] Compiler symbols and IDIV codegen are validated by CI.")
    print("[NOTE] Compile/disassembly checks do not execute guest fault handlers.")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
