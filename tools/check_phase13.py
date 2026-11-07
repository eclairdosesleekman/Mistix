#!/usr/bin/env python3
"""Static source-contract checks for Mistix Phase 13 file seeking."""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def require(path: str, tokens: tuple[str, ...]) -> str:
    target = ROOT / path
    if not target.is_file():
        raise SystemExit(f"missing file: {path}")
    text = target.read_text(encoding="utf-8")
    missing = [token for token in tokens if token not in text]
    if missing:
        raise SystemExit(f"{path}: missing {', '.join(missing)}")
    return text


def main() -> int:
    syscall = require("src/kernel/mistix_syscall.inc", (
        "MISTIX_SYSCALL_LSEEK = 32",
        "VFS_BROKER_SEEK = $4D56534600000015",
        "VFS_BROKER_OP_SEEK = 8",
        "function SyscallLseekUser",
        "VFSBrokerCall(CurrentProcessIndex, VFS_BROKER_OP_SEEK",
        "MISTIX_SYSCALL_LSEEK:",
        "VFS_BROKER_OP_SEEK: Message.MessageId := VFS_BROKER_SEEK",
        "VFS_BROKER_OP_SEEK: Result := Payload",
        "VFSBrokerScratch[I] := Byte(UserBuffer shr (I * 8))",
    ))
    require("src/kernel/mistix_vfs.inc", (
        "function VFSSeekUser",
        "SignedOffset := Int64(Offset)",
        "Magnitude := QWord(-(SignedOffset + 1)) + 1",
        "VFSOpenDescriptions[DescriptionIndex].Position := NewPosition",
    ))
    vfsd = require("userland/vfs/mistixvfsd.pas", (
        "VFS_RPC_BROKER_SEEK = $4D56534600000015",
        "VFS_RPC_BROKER_SEEK:",
        "SeekOffset := Int64(ReadLE64(SharedBuffer, 0))",
        "OpenSlots[Slot].Position := SeekTarget",
        "Response.Words[2] := SeekTarget",
        "Request.MessageId <= VFS_RPC_BROKER_SEEK",
    ))
    if vfsd.count("(Request.MessageId <= VFS_RPC_BROKER_SEEK)") != 2:
        raise SystemExit("vfsd must route SEEK through both broker dispatch paths")
    require("userland/common/mistixlib.inc", (
        "MISTIX_SEEK_SET = 0",
        "MISTIX_SEEK_CUR = 1",
        "MISTIX_SEEK_END = 2",
        "MISTIX_SYSCALL_LSEEK = 32",
        "function SysLseekRaw",
        "function SysLseek(FD: QWord; Offset: Int64; Whence: QWord): QWord;",
        "until Retry >= 1000000;",
    ))
    require("Makefile", ("tools/check_phase13.py", "phase13-check:"))
    require("README.md", (
        "Phase 13 — Remote and local file seeking",
        "SEEK_SET", "private", "make phase13-check",
    ))
    require("docs/PHASE13-DESIGN.md", (
        "signed 64-bit", "private 8 KiB lane", "shared file offset",
        "ESPIPE", "QEMU runtime testing",
    ))
    print("[ OK ] Additive lseek syscall ABI and retrying userland wrapper.")
    print("[ OK ] Local VFS signed-offset arithmetic and shared descriptor cursor.")
    print("[ OK ] Remote SEEK request staged in its isolated Phase 12 lane.")
    print("[ OK ] vfsd validates whence and updates the remote open slot position.")
    print("[NOTE] Static checks do not replace FPC compilation or QEMU runtime tests.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
