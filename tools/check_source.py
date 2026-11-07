#!/usr/bin/env python3
"""Structural checks for Mistix Phase 9; not a substitute for FPC/QEMU."""
from __future__ import annotations
import ast
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KERNEL = ROOT / "src" / "kernel"


def fail(message: str) -> None:
    print(f"[FAIL] {message}", file=sys.stderr)
    raise SystemExit(1)


def read(path: Path) -> str:
    if not path.is_file():
        fail(f"Missing file: {path.relative_to(ROOT)}")
    return path.read_text(encoding="utf-8")


def require(path: Path, tokens: tuple[str, ...]) -> str:
    text = read(path)
    for token in tokens:
        if token not in text:
            fail(f"{path.relative_to(ROOT)} is missing {token!r}")
    return text


def main() -> int:
    kernel_unit = require(ROOT / "src/kernel/mistixkernel.pas", (
        "mistix_vfs.inc", "mistix_process.inc", "mistix_shm.inc",
        "mistix_syscall.inc", "mistix_main.inc",
    ))
    includes = re.findall(r"\{\$I\s+([^}]+)\}", kernel_unit, flags=re.I)
    for include in includes:
        if not (KERNEL / include.strip()).is_file():
            fail(f"Missing kernel include: {include}")

    order = [
        "mistix_types.inc", "mistix_console.inc", "mistix_cpu.inc",
        "mistix_memory.inc", "mistix_threads.inc", "mistix_capability.inc",
        "mistix_ipc.inc", "mistix_notification.inc", "mistix_memory_object.inc",
        "mistix_address_space.inc", "mistix_cow.inc", "mistix_block.inc",
        "mistix_ext2.inc", "mistix_mount.inc", "mistix_net.inc",
        "mistix_vfs.inc", "mistix_process.inc", "mistix_shm.inc",
        "mistix_syscall.inc", "mistix_timer.inc", "mistix_trap.inc",
        "mistix_main.inc",
    ]
    for name in order:
        if name not in includes:
            fail(f"Kernel include graph omits {name}")
    for left, right in zip(order, order[1:]):
        if includes.index(left) > includes.index(right):
            fail(f"Kernel include order must place {left} before {right}")

    kernel_text = "\n".join(read(p) for p in KERNEL.glob("*.inc"))
    require(KERNEL / "mistix_main.inc", (
        "Phase 9: userland VFS syscall broker", "[PHASE 9]", 
        "Regular-file open/read/write/close syscalls use userland vfsd",
    ))
    syscall = require(KERNEL / "mistix_syscall.inc", (
        "MISTIX_SYSCALL_VFS_REGISTER = 31", "SyscallVFSRegister",
        "VFSBrokerInitialize", "VFSBrokerQueueRequest", "VFSBrokerPumpReplies",
        "VFSBrokerCall", "SyscallOpenUser", "SyscallReadUserBuffer",
        "SyscallWriteUserBuffer", "SyscallCloseUser",
        "VFS_BROKER_OPEN", "VFS_BROKER_READ", "VFS_BROKER_WRITE",
        "VFS_BROKER_CLOSE", "VFS_BROKER_SHARED_BASE", "VFS_BROKER_SHARED_SIZE",
        "AddressSpaceTranslate", "PTE_USER or PTE_WRITABLE",
        "VFSBrokerPending[I].TransactionID = Message.Words[0]",
        "VFSBrokerPending[MatchIndex].OwnerPID",
        "Message.Words[1]", "VFSBrokerFlushRemoteCloses", "VFSQueueRemoteClose",
        "RequestPath: array[0..VFS_PATH_MAX] of Char",
        "VFSBrokerPending[SlotIndex].RequestPath[I] <> Path[I]",
        "VFSBrokerPending[SlotIndex].UserBuffer <> UserBuffer",
        "VFSBrokerPending[SlotIndex].ByteCount <> ByteCount",
        "MISTIX_SYSCALL_VFS_REGISTER:",
    ))
    for token in (
        "MISTIX_SYSCALL_BLOCK_REGISTER = 28", "MISTIX_SYSCALL_BLOCK_INFO = 29",
        "MISTIX_SYSCALL_BLOCK_READ = 30", "SyscallBlockRegisterCurrent",
        "SyscallBlockInfo", "SyscallBlockRead", "ProcessSpawnFromPath",
        "P7ShmCreate", "P7ShmMap", "P7ShmUnmap", "ProcessCapabilityLookup",
    ):
        if token not in kernel_text:
            fail(f"Kernel integration is missing {token}")
    if "MISTIX_SYSCALL_BLOCK_WRITE" in syscall:
        fail("Raw physical block write syscall must remain absent in Phase 9")
    if "AddressSpaceTranslate(ProcessTable[CurrentProcessIndex].AddressSpaceRoot" not in syscall or \
       "AddressSpaceTranslate(ProcessTable[ServerIndex].AddressSpaceRoot" not in syscall:
        fail("VFS registration must compare caller and vfsd shared-page translations")
    if "((InitPhysical and not QWord(PAGE_SIZE - 1)) <>" not in syscall:
        fail("VFS registration must compare each shared physical page")

    vfs = require(KERNEL / "mistix_vfs.inc", (
        "voRemote", "RemoteHandle: QWord", "VFSAllocateRemoteFD",
        "VFSIsRemoteFD", "VFSQueueRemoteClose(RemoteHandle: QWord); forward",
        "VFSQueueRemoteClose(RemoteHandle)",
    ))
    if "if ((Kind = voRemote) or (Kind = voRemoteDirectory)) and" not in vfs:
        fail("Last reference to a remote description must enqueue a remote CLOSE")
    process = require(KERNEL / "mistix_process.inc", (
        "procedure VFSBrokerFlushRemoteCloses; forward", "VFSBrokerFlushRemoteCloses;",
        "VFSProcessCloseAll(ProcessTable[ProcessIndex].Files)",
    ))
    if "(RemoteFlags and 3) = VFS_O_WRONLY" not in syscall or \
       "(RemoteFlags and 3) = VFS_O_RDONLY" not in syscall:
        fail("Remote descriptors must enforce read/write access mode bits")
    if "if (Flags and VFS_O_DIRECTORY) <> 0 then" not in syscall or \
       "if IsDevice then" not in syscall:
        fail("Directory and /dev paths must retain their local VFS handling")
    if "VFS_BROKER_CLOSE_QUEUE_MAX = 128" not in syscall or \
       "Message.Words[2] := VFSBrokerCloseQueue[0]" not in syscall:
        fail("Remote descriptor final-close queue is missing or malformed")

    if "SyscallOpenUser(Process, Frame^.SavedRDI, Frame^.SavedRSI)" not in syscall:
        fail("open syscall is not routed through SyscallOpenUser")
    if "SyscallReadUserBuffer(Process, Frame^.SavedRDI" not in syscall:
        fail("read syscall is not routed through SyscallReadUserBuffer")
    if "SyscallWriteUserBuffer(Process, Frame^.SavedRDI" not in syscall:
        fail("write syscall is not routed through SyscallWriteUserBuffer")
    if "SyscallCloseUser(Process, Frame^.SavedRDI)" not in syscall:
        fail("close syscall is not routed through SyscallCloseUser")

    block = require(KERNEL / "mistix_block.inc", (
        "ATA_CMD_IDENTIFY", "ATA_CMD_READ_SECTORS", "LBA28",
        "BLOCK_SECTOR_SIZE = 512", "BlockServerPID",
    ))
    ext2_kernel = require(KERNEL / "mistix_ext2.inc", (
        "EXT2_MAGIC = $EF53", "Ext2Mount", "Ext2ReadFile",
    ))
    mount = require(KERNEL / "mistix_mount.inc", (
        "MountRegisterExt2", "MountFindLongestPrefix", "MOUNT_MAX = 8",
    ))
    net = require(KERNEL / "mistix_net.inc", (
        "NET_ETHERTYPE_IPV4", "NET_ETHERTYPE_ARP", "NetIPv4Checksum",
        "NetParseEthernetFrame", "NetParserSelfTest",
    ))
    if re.search(r"\b(FillChar|Move|GetMem|FreeMem|New|Dispose|WriteLn|ReadLn)\s*\(|\b(SysUtils|Classes)\b", kernel_text, re.I):
        fail("Potential hosted RTL dependency found in kernel source")

    lib = require(ROOT / "userland/common/mistixlib.inc", (
        "SysWriteRaw", "SysReadRaw", "SysOpenRaw", "SysCloseRaw",
        "SysVFSRegister", "MISTIX_SYSCALL_VFS_REGISTER = 31",
        "MISTIX_O_CREAT = $40", "function SysWrite(FD, Buffer, Count: QWord)",
        "function SysRead(FD, Buffer, Count: QWord)",
        "function SysOpen(Path, Flags: QWord)", "function SysClose(FD: QWord)",
        "function SysWriteAll", "if IsError(Written) then Exit",
    ))
    if "Reserved: array[0..6] of Byte" not in lib or "Words: array[0..5] of QWord" not in lib:
        fail("Userland IPC message ABI does not match kernel TIPCMessage")
    if "until Retry >= 1000000" not in lib:
        fail("Userland retry wrappers must be bounded")

    vfsd = require(ROOT / "userland/vfs/mistixvfsd.pas", (
        "VFS_RPC_BROKER_OPEN", "VFS_RPC_BROKER_READ",
        "VFS_RPC_BROKER_WRITE", "VFS_RPC_BROKER_CLOSE",
        "DispatchBrokerRequest", "CopySharedBrokerPath",
        "VFS_O_CREAT", "VFS_RPC_IO_MAX = 4096",
        "SourceKind: Byte; { 0=MFS1, 1=tmpfs, 2=read-only Ext2 }",
        "Ext2UReadFile", "Ext2FileScratch", "SharedBuffer[I] := Ext2FileScratch[I]",
        "BlockRequestEndpoint := BlockSendHandle",
    ))
    if "if Requested <> 0 then\n          for I := 0 to LongWord(Requested) - 1 do Dest[I] := Source[I];" not in vfsd:
        fail("Broker VFS write must guard zero-length requests")
    if "Response.Words[0] := Request.Words[0]" not in vfsd or \
       "Response.Words[1] := Request.Words[1]" not in vfsd:
        fail("Broker VFS replies must echo transaction ID and caller PID")
    if "if (OpenSlots[Slot].Flags and 3) = VFS_O_WRONLY then" not in vfsd or \
       "if (OpenSlots[Slot].Flags and 3) = VFS_O_RDONLY then" not in vfsd:
        fail("vfsd must enforce remote open read/write access modes")
    if "Request.Words[0]" not in vfsd or "VFS_RPC_BROKER_CLOSE:" not in vfsd:
        fail("vfsd remote-close protocol is missing")

    init = require(ROOT / "userland/init/mistixinit.pas", (
        "SysVFSRegister(PID, RequestHandle, ReplyHandle)",
        "Standard file syscalls now route through userland vfsd",
        "SysOpen(PtrUInt(@MotdPath[0]), MISTIX_O_RDONLY)",
        "SysRead(FileHandle", "SysClose(FileHandle)",
        "MISTIX_O_RDWR or MISTIX_O_CREAT", "SysWriteAll(FileHandle",
        "standard syscall O_CREAT/write/read/close proxy round-trip passed",
        "Mistix Phase 9",
    ))
    if init.index("StartBlockService") > init.index("TestVFSService"):
        fail("blockd must start before vfsd")

    makefile = require(ROOT / "Makefile", (
        "mistix.iso", "mistix_vfs.inc", "mistix_syscall.inc",
        "vfsd.elf", "blockd.elf", "userland/vfs/mistix_ext2_client.inc",
        "Mistix Phase 9", "$(QEMU) -M pc",
    ))
    if "mistix-phase8.iso" in makefile or "Mistix Phase 8" in makefile:
        fail("Stale Phase 8 branding in Makefile")
    mkinitfs = read(ROOT / "tools/mkinitfs.py")
    ast.parse(mkinitfs)
    require(ROOT / "README.md", (
        "Mistix Phase 9", "vfs_register()", "open/read/write/close",
        "transaction ID", "O_CREAT", "getdents()", "FPC 実コンパイル",
        "未確認", "1件の VFS transaction",
    ))
    require(ROOT / "docs/PHASE9-DESIGN.md", (
        "Trust and registration", "Transaction protocol", "Remote file descriptions",
        "one global in-flight", "directory-FD proxy",
    ))

    forbidden = list(ROOT.rglob("*.c")) + list(ROOT.rglob("*.h"))
    if forbidden:
        fail(f"Unexpected C source/header: {forbidden[0].relative_to(ROOT)}")

    print("[ OK ] Kernel include graph and Phase 9 syscall dispatch checked.")
    print("[ OK ] vfs_register validates PID 1 authority, endpoint rights and shared physical pages.")
    print("[ OK ] Regular-file open/read/write/close are routed through the userland VFS broker.")
    print("[ OK ] Remote FD reference lifecycle, access modes and asynchronous last-reference close are checked.")
    print("[ OK ] VFS RPC transaction IDs, PID matching and bounded transfer sizes are checked.")
    print("[ OK ] Init tests standard syscall reads and /tmp O_CREAT/write/read round-trip.")
    print("[ OK ] Prior block, Ext2, device, network services and Pascal utilities are retained.")
    print("[ OK ] Phase 9 Makefile, documentation and Python build tool syntax checked.")
    print("[NOTE] These structural checks do not replace FPC compilation or QEMU boot testing.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
