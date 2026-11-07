#!/usr/bin/env python3
"""Static source-contract checks for Mistix Phase 17 runtime fault isolation."""
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
        "tools/check_phase17.py",
        "phase17-check:",
        "RUNTIMEFAULT_ELF := $(USER_BUILD_DIR)/runtimefault.elf",
        "userland/tests/mistixruntimefault.pas",
        "--file /bin/runtimefault=$(RUNTIMEFAULT_ELF)",
        "Runtime-fault isolation marker missing",
        '[ "$(BUILD_TYPE)" = "debug" ]',
    ))
    if makefile.count("tools/check_phase17.py") != 2:
        raise SystemExit("Phase 17 checker must run from check-source and phase17-check.")

    require("userland/common/mistixlib.inc", (
        "public name 'FPC_DIVBYZERO'",
        "public name 'FPC_RANGEERROR'",
        "public name 'FPC_OVERFLOW'",
        "SysExit(200)", "SysExit(201)", "SysExit(215)",
    ))
    require("userland/tests/mistixruntimefault.pas", (
        "TProbeIndex = 0..3",
        "ProbeTable[IndexValue] := Byte(IndexValue)",
        "LeftValue := High(LongInt)",
        "ArithmeticResult := LeftValue + RightValue",
        "Denominator := Ord(Mode[2]) - Ord(Mode[2])",
        "Numerator div Denominator",
        "SysGetCommandLine",
    ))
    require("userland/init/mistixinit.pas", (
        "function RunRuntimeFaultCase",
        "SysSpawnPath", "SysWaitPID",
        "RuntimeRangeArgs, RuntimeRangeName, 201",
        "RuntimeOverflowArgs, RuntimeOverflowName, 215",
        "RuntimeDivideArgs, RuntimeDivideName,",
        "QWord($8000000000000000)",
        "MISTIX_DEBUG_CHECKS",
        "[ OK ] Phase 17 runtime fault isolation tests passed.",
    ))
    require("src/kernel/mistix_trap.inc", (
        "ExitCode := QWord($8000000000000000) or",
        "ProcessTerminateCurrent(Frame, ExitCode)",
        "if (Frame^.CS and 3) = 3 then",
    ))
    require("userland/init/mistixinit.pas", (
        "VFS_RPC_READDIR = $4D5653460000000A",
        "call MistixInitMain",
    ))
    for source, entry in (
        ("userland/sh/mistixsh.pas", "MistixShellMain"),
        ("userland/bin/mistixls.pas", "MistixLSMain"),
        ("userland/bin/mistixcat.pas", "MistixCatMain"),
        ("userland/bin/mistixecho.pas", "MistixEchoMain"),
        ("userland/vfs/mistixvfsd.pas", "MistixVFSdMain"),
        ("userland/block/mistixblockd.pas", "MistixBlockdMain"),
        ("userland/device/mistixdevd.pas", "MistixDevdMain"),
        ("userland/network/mistixnetd.pas", "MistixNetdMain"),
        ("userland/tests/mistixruntimefault.pas", "MistixRuntimeFaultMain"),
    ):
        require(source, (f"call {entry}",))
    require("docs/PHASE17-DESIGN.md", (
        "FPC_DIVBYZERO", "fpc_rangeerror", "FPC_RANGEERROR", "FPC_OVERFLOW",
        "201", "215", "200", "IDIV", "0x8000000000000000",
        "SysWaitPID", "make smoke BUILD_TYPE=debug",
        "not a hardware qualification",
    ))
    require("docs/PHASE16-DESIGN.md", (
        "Phase 17 adds", "does not execute those handlers",
    ))
    require("README.md", (
        "Phase 17 — runtime fault isolation",
        "make phase17-check",
        "docs/PHASE17-DESIGN.md",
    ))
    print("[ OK ] Userland runtime hooks map compiler faults to process exit statuses.")
    print("[ OK ] Debug init spawns, waits for, and validates all three fault cases.")
    print("[ OK ] QEMU Debug smoke requires the Phase 17 success marker.")
    print("[NOTE] These targeted guest tests do not validate kernel-mode panic handling.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
