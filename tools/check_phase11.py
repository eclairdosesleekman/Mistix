#!/usr/bin/env python3
"""Static source-contract checks for Mistix Phase 11 directory broker wiring."""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def require(path: str, tokens: tuple[str, ...]) -> str:
    file_path = ROOT / path
    if not file_path.is_file():
        raise SystemExit(f"missing file: {path}")
    text = file_path.read_text(encoding="utf-8")
    missing = [token for token in tokens if token not in text]
    if missing:
        raise SystemExit(f"{path}: missing {', '.join(missing)}")
    return text


def main() -> int:
    require("src/kernel/mistix_vfs.inc", (
        "voRemoteDirectory", "VFSAllocateRemoteDirectoryFD",
        "VFSIsRemoteDirectoryFD", "VFSQueueRemoteClose(RemoteHandle)",
        "if (Kind = voDirectory) or (Kind = voRemoteDirectory) or",
    ))
    syscall = require("src/kernel/mistix_syscall.inc", (
        "VFS_BROKER_DIR_OPEN = $4D56534600000021",
        "VFS_BROKER_DIR_READ = $4D56534600000022",
        "VFS_BROKER_CHDIR = $4D56534600000024",
        "VFS_BROKER_OP_DIR_OPEN = 5", "VFS_BROKER_OP_DIR_READ = 6",
        "VFS_BROKER_OP_CHDIR = 7", "VFSBrokerIsExt2Path",
        "VFSBrokerCall(CurrentProcessIndex, VFS_BROKER_OP_DIR_READ",
        "VFSBrokerCall(CurrentProcessIndex, VFS_BROKER_OP_CHDIR",
        "SyscallGetDentsUser(Process", "SyscallChdirUser(Process",
        "ValidEntries", "NameTerminated",
    ))
    if "MISTIX_SYSCALL_GETDENTS:\n      ReturnValue := VFSGetDentsUser" in syscall:
        raise SystemExit("getdents dispatch bypasses remote-directory routing")
    require("userland/vfs/mistixvfsd.pas", (
        "VFS10_DIR_OPEN = $4D56534600000021",
        "VFS10_DIR_READ = $4D56534600000022",
        "VFS10_CHDIR = $4D56534600000024",
        "SourceKind <> 3", "Ext2UReadDirectory(OpenSlots[Slot].Path",
        "VFS_ERR_NOTDIR", "OpenSlots[Slot].Path[I] := Path[I]",
        "VFS_O_DIRECTORY = $100",
    ))
    require("userland/vfs/mistix_ext2_client.inc", (
        "SkipEntry := True", "Ext2BlockScratch[EntryOffset + 8 + I] = Ord('/')",
        "Ext2BlockScratch[EntryOffset + 8 + I] < 32",
    ))
    require("userland/common/mistixlib.inc", (
        "function SysGetDentsRaw", "function SysChdirRaw",
        "function SysGetDents(FD, Buffer, MaxEntries: QWord): QWord;",
        "function SysChdir(Path: QWord): QWord;", "until Retry >= 1000000;",
    ))
    require("Makefile", ("tools/check_phase11.py", "phase11-check:"))
    require("docs/PHASE11-DESIGN.md", (
        "voRemoteDirectory", "single-slot transaction", "getcwd()",
        "128 visible entries", "CANCEL acknowledgement",
    ))
    require("README.md", (
        "Phase 11 — Userland Ext2 directory descriptors",
        "make phase11-check", "getcwd()",
    ))
    print("[ OK ] Remote-directory descriptor allocation and final-close lifecycle.")
    print("[ OK ] getdents and chdir route Ext2 /mnt/disk operations through vfsd.")
    print("[ OK ] userland wrappers retry the serialized broker's EAGAIN.")
    print("[ OK ] Ext2 dot entries and unsafe names are filtered before copy-out.")
    print("[ OK ] Phase 11 documentation and Makefile check target are wired.")
    print("[NOTE] Static checks do not replace FPC compilation or QEMU runtime testing.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
