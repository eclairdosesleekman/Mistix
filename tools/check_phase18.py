#!/usr/bin/env python3
"""Static source-contract checks for Mistix Phase 18 user page-fault isolation."""
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
        "phase18-check:",
        "tools/check_phase18.py",
        "User page-fault isolation marker missing",
        "qemu_status=$$?; set -e",
        "if [ $$qemu_status -ne 124 ] && [ $$qemu_status -ne 0 ]",
        '[ "$(BUILD_TYPE)" = "debug" ]',
    ))
    if makefile.count("tools/check_phase18.py") != 2:
        raise SystemExit("Phase 18 checker must run from check-source and phase18-check.")

    probe = require("userland/tests/mistixruntimefault.pas", (
        "procedure ExecuteProbeAddress(Address: PtrUInt); cdecl; assembler; nostackframe",
        "call rdi",
        "PByte(PtrUInt($0000600000000000))^",
        "PByte(PtrUInt(@MistixRuntimeFaultMain))^ := $90",
        "CodeByte := $C3",
        "ExecuteProbeAddress(PtrUInt(@CodeByte))",
        "'p':",
        "'r':", "'w':", "'t':", "'x':",
    ))
    if probe.count("PByte(PtrUInt($0000600000000000))^") != 2:
        raise SystemExit("The unmapped user page must be probed once for read and once for write.")

    require("userland/init/mistixinit.pas", (
        "PAGE_FAULT_STATUS_BASE = not QWord($7FFFFFF1FFFFFFFF)",
        "RuntimePageReadArgs", "RuntimePageWriteArgs",
        "RuntimePageTextWriteArgs", "RuntimePageExecNXArgs",
        "PAGE_FAULT_STATUS_BASE or 4",
        "PAGE_FAULT_STATUS_BASE or 6",
        "PAGE_FAULT_STATUS_BASE or 7",
        "PAGE_FAULT_STATUS_BASE or $15",
        "SysSpawnPath", "SysWaitPID",
        "[ OK ] Phase 17 runtime fault isolation tests passed.",
        "[ OK ] Phase 18 user-mode page-fault isolation tests passed.",
    ))
    require("src/kernel/mistix_trap.inc", (
        "if (Vector = 14) and (CurrentProcessIndex < PROCESS_MAX_COUNT) then",
        "ExitCode := QWord($8000000000000000) or (Vector shl 32) or",
        "(Frame^.ErrorCode and $FFFFFFFF)",
        "ProcessTerminateCurrent(Frame, ExitCode)",
    ))
    require("src/kernel/mistix_types.inc", (
        "PTE_NO_EXECUTE = not QWord($7FFFFFFFFFFFFFFF)",
    ))
    require("src/kernel/mistix_address_space.inc", (
        "ProgramHeader^.Flags and (ELF_PF_WRITE or ELF_PF_EXECUTE)",
        "PageFlags := PTE_USER;",
        "PageFlags := PageFlags or PTE_WRITABLE;",
        "PageFlags := PageFlags or PTE_NO_EXECUTE;",
        "StagingFlags := PTE_USER or PTE_WRITABLE or PTE_NO_EXECUTE",
        "if not AddressSpaceMapZeroPage(RootPhysical, PageAddress,",
        "if not AddressSpaceCopyTo(RootPhysical, ProgramHeader^.VirtualAddress,",
        "if not AddressSpaceSetPageFlags(RootPhysical, PageAddress,",
        "function AddressSpaceSetPageFlags(RootPhysical, VirtualAddress,",
        "((Flags and PTE_WRITABLE) <> 0) and",
        "((Flags and PTE_NO_EXECUTE) = 0) then Exit",
        "if (ReadCR3 and PTE_ADDRESS_MASK) = RootPhysical then Exit",
        "((Flags and PTE_WRITABLE) <> 0) and",
        "((Flags and PTE_NO_EXECUTE) = 0)",
        "Flags := PTE_USER or PTE_WRITABLE;",
        "Flags := Flags or PTE_NO_EXECUTE;",
    ))
    require("README.md", (
        "The latest source and design milestone in the repository is **Phase 18**.",
        "Phase 18 — user-mode page-fault isolation",
        "make phase18-check",
        "FPC 実コンパイル",
        "docs/PHASE18-DESIGN.md",
    ))
    require("docs/PHASE18-DESIGN.md", (
        "0x8000000E00000004",
        "0x8000000E00000006",
        "0x8000000E00000007",
        "0x8000000E00000015",
        "NX", "COW",
        "make smoke BUILD_TYPE=debug",
        "not exhaustive MMU testing",
    ))
    print("[ OK ] Debug probe covers unmapped read/write, read-only text write, and NX stack execution.")
    print("[ OK ] PID 1 validates the exact user page-fault vector and P/W/U/I-D status bits.")
    print("[ OK ] Debug QEMU smoke requires the Phase 18 page-fault isolation marker.")
    print("[NOTE] These controlled tests do not constitute exhaustive MMU or hardware qualification.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
