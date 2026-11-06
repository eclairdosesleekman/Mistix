#!/usr/bin/env python3
"""Structural checks for Mistix Phase 8; this is not an FPC compiler."""
from __future__ import annotations
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
        "mistix_block.inc", "mistix_ext2.inc", "mistix_mount.inc",
        "mistix_net.inc", "mistix_vfs.inc", "mistix_shm.inc",
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
    for token in (
        "BlockInitialize", "BlockReadSectors", "BlockWriteSectors",
        "BlockScanMBR", "ATA_CMD_IDENTIFY", "ATA_CMD_READ_SECTORS",
        "Ext2Mount", "Ext2ReadInode", "Ext2LookupPath", "Ext2ReadFile",
        "MountRegisterExt2", "MountFindLongestPrefix", "NetParseEthernetFrame",
        "NetIPv4Checksum", "VFSInitialize", "ProcessSpawnFromPath",
        "SyscallDispatchFrame", "TrapDispatch", "P7ShmCreate", "P7ShmMap",
        "P7ShmUnmap", "P7ShmReleaseProcess", "P7ShmProcessHasMappings",
    ):
        if not re.search(rf"\b{re.escape(token)}\b", kernel_text):
            fail(f"Kernel is missing {token}")

    block = require(KERNEL / "mistix_block.inc", (
        "ATA_CMD_IDENTIFY", "ATA_CMD_READ_SECTORS", "ATA_CMD_WRITE_SECTORS",
        "LBA28", "BlockScanMBR", "BLOCK_SECTOR_SIZE = 512",
        "BlockServerPID", "BlockServerPID := 0",
    ))
    syscall = require(KERNEL / "mistix_syscall.inc", (
        "MISTIX_SYSCALL_BLOCK_REGISTER = 28",
        "MISTIX_SYSCALL_BLOCK_INFO = 29",
        "MISTIX_SYSCALL_BLOCK_READ = 30",
        "SyscallBlockRegisterCurrent", "SyscallBlockInfo", "SyscallBlockRead",
        "ProcessTable[CurrentProcessIndex].ParentPID <> 1",
        "SectorCount > 128", "AddressSpaceValidateUserRange",
        "MISTIX_SYSCALL_BLOCK_READ:",
    ))
    if "MISTIX_SYSCALL_BLOCK_WRITE" in syscall:
        fail("Raw physical block write syscall must remain absent in Phase 8")

    ext2_kernel = require(KERNEL / "mistix_ext2.inc", (
        "EXT2_MAGIC = $EF53", "EXT2_FEATURE_INCOMPAT_FILETYPE",
        "Ext2Mount", "Ext2ReadFile", "LogicalBlock >= 12",
        "EXT2_FEATURE_COMPAT_HAS_JOURNAL",
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
        "TMistixIPCMessage", "SysEndpointCreate", "SysIPCSend",
        "SysIPCReceive", "SysCapabilityTransfer", "SysCapabilityReceive",
        "SysCapabilityClose", "SysSpawnPath", "SysWriteAll",
        "SysShmCreate", "SysShmMap", "SysShmUnmap",
        "MISTIX_SYSCALL_SHM_CREATE", "MISTIX_SYSCALL_BLOCK_REGISTER",
        "MISTIX_SYSCALL_BLOCK_INFO", "MISTIX_SYSCALL_BLOCK_READ",
        "SysBlockRegister", "SysBlockInfo", "SysBlockRead",
    ))
    if "Reserved: array[0..6] of Byte" not in lib or "Words: array[0..5] of QWord" not in lib:
        fail("Userland IPC message ABI does not match the kernel TIPCMessage")
    if "SectorCount: QWord" not in lib or "SectorSize: LongWord" not in lib:
        fail("Block info ABI layout is missing")

    blockd = require(ROOT / "userland/block/mistixblockd.pas", (
        "SysBlockRegister", "SysBlockInfo", "SysBlockRead",
        "HardwareReadBytes", "BLOCK_RPC_INFO", "BLOCK_RPC_READ",
        "BLOCK_RPC_WRITE", "BLOCK_ERR_ROFS", "RAMDisk",
        "BatchSectors > 128", "HardwareScratch",
    ))
    hardware_write = re.search(r"BLOCK_RPC_WRITE:\s*begin(?P<body>.*?)\n\s*end;", blockd, re.S)
    if not hardware_write or "BLOCK_ERR_ROFS" not in hardware_write.group("body"):
        fail("Physical ATA writes must be rejected with EROFS")

    ext2_types = require(ROOT / "userland/vfs/mistix_ext2_types.inc", (
        "TExt2UserInode", "Ext2DiskBytes", "Ext2DirEntries",
        "BlockRequestEndpoint", "BlockReplyEndpoint",
    ))
    ext2_client = require(ROOT / "userland/vfs/mistix_ext2_client.inc", (
        "Ext2UBlockRPCInfo", "Ext2UBlockRPCRead", "Ext2UMount",
        "EXT2_U_MAGIC", "EXT2_U_FEATURE_COMPAT_HAS_JOURNAL",
        "EXT2_U_FEATURE_INCOMPAT_FILETYPE", "EXT2_U_FEATURE_COMPAT_DIR_INDEX",
        "RootInode", "Ext2UReadInode(InodeNumber: LongWord; out Inode: TExt2UserInode): Boolean; forward;",
        "Ext2UResolveBlock", "Inode.Blocks[12]", "Ext2ULookupPath",
        "Ext2UReadDirectory", "Ext2UReadFile", "Ext2UPathIsMounted",
        "LogicalBlocks = 0", "NameLength > 63",
    ))
    if "Status := SysIPCReceive(BlockReplyEndpoint" not in ext2_client:
        fail("Ext2 client must receive blockd replies over IPC")
    if "SysYield" not in ext2_client:
        fail("Ext2 client retry loop must yield while awaiting blockd")

    vfsd = require(ROOT / "userland/vfs/mistixvfsd.pas", (
        "VFS_RPC_MOUNT_EXT2", "VFS_RPC_READDIR", "mistix_ext2_types.inc",
        "mistix_ext2_client.inc", "VFS_ERR_NOTDIR", "VFS_ERR_IO",
        "Ext2UMount", "Ext2UReadDirectory", "Ext2UReadFile",
        "SourceKind: Byte; { 0=MFS1, 1=tmpfs, 2=read-only Ext2 }",
        "BlockRequestEndpoint := BlockSendHandle",
    ))
    if "Ext2UReadFile(LongWord(OpenSlots[Slot].DataOffset)" not in vfsd or \
       "@Ext2FileScratch[0]" not in vfsd or "SharedBuffer[I] := Ext2FileScratch[I]" not in vfsd:
        fail("VFS read path must stage Ext2 data separately from the block RPC window")

    init = require(ROOT / "userland/init/mistixinit.pas", (
        "StartBlockService", "TestBlockService", "TestVFSService",
        "BlockdPath", "VFSdPath", "SysCapabilityTransfer",
        "BlockRequestHandle", "BlockReplyHandle", "VFS_RPC_MOUNT_EXT2",
        "VFS RPC v3 handshake", "BLOCK_RPC_READ",
    ))
    if init.index("StartBlockService") > init.index("TestVFSService"):
        fail("blockd must be started before vfsd mount negotiation")

    for name, path in {
        "shell": ROOT / "userland/sh/mistixsh.pas",
        "ls": ROOT / "userland/bin/mistixls.pas",
        "cat": ROOT / "userland/bin/mistixcat.pas",
        "echo": ROOT / "userland/bin/mistixecho.pas",
    }.items():
        text = read(path)
        if "mistixlib.inc" not in text or "mistix_user_main" not in text:
            fail(f"{name} is not a Pascal syscall-based user program")

    makefile = require(ROOT / "Makefile", (
        "mistix-phase8.iso", "mistix_block.inc", "mistix_ext2.inc",
        "vfs_base.img", "vfsd.elf", "blockd.elf", "devd.elf", "netd.elf",
        "userland/vfs/mistix_ext2_types.inc",
        "userland/vfs/mistix_ext2_client.inc",
        "_binary_initfs_img_start", "userland/vfs/mistixvfsd.pas",
        "$(QEMU) -M pc",
    ))
    if "Mistix Phase 7" in makefile or "mistix-phase7.iso" in makefile:
        fail("Stale Phase 7 branding in Makefile")
    require(ROOT / "tools/mkinitfs.py", ("MFS1", "MFS1 supports at most 48 files"))
    require(ROOT / "README.md", (
        "Phase 8", "block_register()", "blockd", "Ext2", "/mnt/disk",
        "読み取り専用", "FPC 実コンパイル", "未確認",
        "標準ファイル syscall 全体の broker 化は完了していません",
    ))
    require(ROOT / "docs/PHASE8-DESIGN.md", (
        "Trust boundary", "blockd", "Ext2 mount policy",
        "Why regular syscalls are not yet brokered",
    ))

    forbidden = list(ROOT.rglob("*.c")) + list(ROOT.rglob("*.h"))
    if forbidden:
        fail(f"Unexpected C source/header: {forbidden[0].relative_to(ROOT)}")

    print("[ OK ] Kernel include graph and Phase 8 syscall integration checked.")
    print("[ OK ] Block service registration, ATA read-only syscall and RAM fallback checked.")
    print("[ OK ] Shared-memory block RPC and bounded multi-batch sector reads checked.")
    print("[ OK ] Userland Ext2 mount, inode/directory lookup and file read paths checked.")
    print("[ OK ] VFS mount/read-dir RPC and init capability bootstrap checked.")
    print("[ OK ] Existing Pascal shell, utilities, VFS overlay and network services retained.")
    print("[ OK ] Makefile dependencies and Phase 8 documentation checked.")
    print("[NOTE] Structural checks do not replace FPC compilation or QEMU hardware testing.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
