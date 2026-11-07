#!/usr/bin/env python3
"""Static source-contract checks for Mistix Phase 15 runtime diagnostics."""
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
        "FPC_DIAGNOSTIC_FLAGS ?= -g",
        "FPC_RUNTIME_CHECK_FLAGS ?= -Cr -Co -dMISTIX_DEBUG_CHECKS",
        "tools/check_phase15.py",
        "phase15-check:",
        "mistix_runtime.inc",
    ))
    if makefile.count("tools/check_phase15.py") != 2:
        raise SystemExit("Phase 15 checker must be wired into check-source and its target.")

    kernel = require("src/kernel/mistixkernel.pas", (
        "{$ifdef MISTIX_DEBUG_CHECKS}",
        "{$R+}",
        "{$Q+}",
        "{$I mistix_runtime.inc}",
    ))
    if kernel.count("{$R-}") != 1 or kernel.count("{$Q-}") != 1:
        raise SystemExit("Kernel Release fallback directives must appear exactly once.")

    require("src/kernel/mistix_runtime.inc", (
        "public name 'FPC_RANGEERROR'",
        "public name 'fpc_rangeerror'",
        "public name 'FPC_OVERFLOW'",
        "public name 'fpc_overflow'",
        "public name 'FPC_DIVBYZERO'",
        "MistixRuntimePanic(201)",
        "MistixRuntimePanic(215)",
        "MistixRuntimePanic(200)",
        "mov dx, $3FD",
        "out dx, al",
        "hlt",
    ))
    require("userland/common/mistixlib.inc", (
        "{$ifdef MISTIX_DEBUG_CHECKS}",
        "{$R+}", "{$Q+}", "{$R-}", "{$Q-}",
        "public name 'FPC_DIVBYZERO'",
        "public name 'FPC_RANGEERROR'",
        "public name 'fpc_rangeerror'",
        "public name 'FPC_OVERFLOW'",
        "public name 'fpc_overflow'",
        "SysExit(200)", "SysExit(201)", "SysExit(215)",
        "mov eax, MISTIX_SYSCALL_EXIT", "int $80", "ud2",
    ))
    require("README.md", (
        "Phase 15 — Freestanding runtime diagnostics",
        "FPC_RANGEERROR",
        "make phase15-check",
        "docs/PHASE15-DESIGN.md",
    ))
    require("docs/PHASE15-DESIGN.md", (
        "FPC_DIVBYZERO", "FPC_RANGEERROR", "FPC_OVERFLOW",
        "201", "215", "200", "-Cr -Co", "does not enable stack checks",
    ))
    require(".github/workflows/ci.yml", (
        "Boot release profile in QEMU",
        "Boot checked debug profile in QEMU",
        "make smoke BUILD_TYPE=debug",
        "build/debug/qemu-serial.log",
    ))

    print("[ OK ] Debug enables FPC range/overflow checks; Release remains unchecked.")
    print("[ OK ] Kernel supplies freestanding serial panic handlers.")
    print("[ OK ] Userland terminates the faulting process with the matching status.")
    print("[ OK ] CI boots both Release and checked Debug profiles.")
    print("[NOTE] Static checks and QEMU smoke do not replace fault injection or hardware validation.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
