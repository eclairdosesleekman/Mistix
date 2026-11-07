#!/usr/bin/env python3
"""Static source-contract checks for Mistix Phase 12 VFS concurrency."""
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
    kernel = require("src/kernel/mistix_syscall.inc", (
        "VFS_BROKER_PENDING_MAX = 8",
        "VFS_BROKER_SLOT_SIZE = 8192",
        "VFS_BROKER_TIMEOUT_TICKS = 500",
        "VFS_BROKER_CANCEL_GRACE_TICKS = 1000",
        "VFSBrokerServerGeneration",
        "VFSBrokerPending: array[0..VFS_BROKER_PENDING_MAX - 1]",
        "procedure SyscallInitialize;",
        "function SyscallVFSRegister(ServerPID", 
        "function VFSBrokerHasActiveRequests",
        "procedure VFSBrokerTrySendCancel",
        "Message.Words[4] := SlotIndex",
        "Message.Words[5] := VFSBrokerServerGeneration",
        "VFSBrokerPending[I].TransactionID = Message.Words[0]",
        "VFSBrokerPending[MatchIndex].ServerGeneration",
        "VFS_BROKER_ERR_TIMEOUT", "TimeoutDelivered",
        "AddressSpaceCopyTo(ServerRoot, LaneAddress + VFS_IO_MAX",
    ))
    require("src/kernel/mistix_process.inc", (
        "procedure VFSBrokerFlushRemoteCloses; forward;",
    ))
    if "VFSBrokerPending.Active" in kernel:
        raise SystemExit("legacy singleton VFS broker state remains")
    trap = require("src/kernel/mistix_trap.inc", (
        "Inc(TimerTickCount);", "Inc(VFSBrokerTickCounter);",
    ))
    vfsd = require("userland/vfs/mistixvfsd.pas", (
        "VFS10_CANCEL = $4D56534600000026",
        "VFS_BROKER_PENDING_MAX = 8",
        "VFS_BROKER_SLOT_SIZE = 8192",
        "function VFSBrokerStageRequest",
        "procedure VFSBrokerSaveResponse",
        "Request.Words[5]",
        "Response.Words[3] := Request.Words[5]",
        "Response.MessageId := VFS10_CANCEL",
        "SharedBuffer[BufferOffset + I] := SharedBuffer[I]",
    ))
    if "for I := 0 to LongWord(CopyBytes) - 1 do" not in vfsd:
        raise SystemExit("response lane copy loop missing")
    require("Makefile", (
        "tools/check_phase12.py",
        "phase12-check:",
    ))
    require("README.md", (
        "Phase 12 — Multi-request VFS broker",
        "eight outstanding transactions",
        "five-second deadline", "no separate watchdog thread",
    ))
    require("docs/PHASE12-DESIGN.md", (
        "eight outstanding transactions",
        "server generations",
        "CANCEL is best-effort",
        "single-threaded service",
        "grace period",
    ))
    print("[ OK ] Eight-entry request table and per-slot 8 KiB shared-memory lanes.")
    print("[ OK ] Replies match transaction ID, owner PID and server generation.")
    print("[ OK ] PIT-driven timeout, CANCEL notification and late-reply retirement.")
    print("[ OK ] vfsd stages request lanes and preserves read results per transaction.")
    print("[NOTE] Static checks do not replace FPC compilation or QEMU runtime tests.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
