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
        "public name 'fpc_overflow'",
        "SysExit(200)", "SysExit(201)", "SysExit(215)",
    ))
    require("userland/tests/mistixruntimefault.pas", (
        "TProbeIndex = 0..3",
        "ProbeTable[IndexValue] := Byte(IndexValue)",
        "LeftValue := High(Int64)",
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
    require("src/kernel/mistix_cpu.inc", (
        "jmp ISRCommon",
        "call TrapDispatch",
        "function TrapDispatch(Frame: PInterruptFrame): QWord; cdecl; forward",
        "procedure ISRCommon; assembler; nostackframe; forward",
    ))
    require("src/kernel/mistix_syscall.inc", ("jmp ISRCommon",))
    require("src/kernel/mistix_timer.inc", ("jmp ISRCommon",))
    require("src/kernel/mistix_address_space.inc", (
        "function AddressSpaceActivate(RootPhysical: QWord): Boolean",
        "WriteCR3(RootPhysical)",
        "(ReadCR3 and PTE_ADDRESS_MASK) = RootPhysical",
    ))
    require("src/kernel/mistix_types.inc", (
        "VMM_TEST_VIRTUAL_ADDRESS = QWord($FFFFC00000000000)",
    ))
    require("src/kernel/mistix_types.inc", (
        "{$push}", "{$R-}", "LimineRequests: TLimineRequestBlock = (",
        "public name 'limine_request_block'", "{$pop}",
    ))
    require("src/kernel/mistix_memory_object.inc", (
        "MEMORY_OBJECT_TEST_BASE = QWord($FFFFC10000000000)",
    ))
    syscall_source = require("src/kernel/mistix_syscall.inc", (
        "MISTIX_ERR_NOSYS = not QWord(37)",
        "MISTIX_ERR_FAULT = not QWord(13)",
        "MISTIX_ERR_INVAL = not QWord(21)",
        "MISTIX_ERR_NOMEM = not QWord(11)",
        "MISTIX_ERR_BUSY = not QWord(15)",
    ))
    if "QWord(-" in syscall_source:
        raise SystemExit("signed negative QWord casts remain in checked kernel syscall paths")
    for source, expression in (
        ("src/kernel/mistix_memory.inc", "Words^[I] := 0;"),
        ("src/kernel/mistix_memory_object.inc", "Words^[I] := 0;"),
        ("src/kernel/mistix_shm.inc", "Words^[J] := 0;"),
        ("src/kernel/mistix_threads.inc", "StackWords^[6] := PtrUInt(@ThreadBootstrap);"),
    ):
        require(source, (expression,))
    if "VFSBrokerFlushRemoteCloses; forward;" in require(
        "src/kernel/mistix_syscall.inc", ("procedure VFSBrokerFlushRemoteCloses;",)
    ):
        raise SystemExit("duplicate VFSBrokerFlushRemoteCloses forward remains in syscall include")
    require("src/kernel/mistix_main.inc", (
        "call KernelMain",
        "procedure KernelMain; cdecl",
    ))
    require("userland/init/mistixinit.pas", (
        "VFS_RPC_READDIR = $4D5653460000000A",
        "call MistixInitMain",
    ))
    require("userland/sh/mistixsh.pas", (
        "HelpText: array[0..62] of Char = 'Builtins: help, cd PATH, pwd, exit. Commands: ls, cat, echo.'",
        "call MistixShellMain",
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
