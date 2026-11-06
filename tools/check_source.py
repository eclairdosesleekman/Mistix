#!/usr/bin/env python3
"""Structural checks for Mistix Phase 7. This is not an FPC compiler."""
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
        "mistix_net.inc", "mistix_vfs.inc", "mistix_shm.inc", "mistix_syscall.inc",
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
        "mistix_vfs.inc", "mistix_process.inc", "mistix_shm.inc", "mistix_syscall.inc",
        "mistix_timer.inc", "mistix_trap.inc", "mistix_main.inc",
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
        "P7ShmUnmap", "P7ShmReleaseProcess", "okMemoryObject",
    ):
        if not re.search(rf"\b{re.escape(token)}\b", kernel_text):
            fail(f"Kernel is missing {token}")

    block = require(KERNEL / "mistix_block.inc", (
        "ATA_CMD_IDENTIFY", "ATA_CMD_READ_SECTORS", "ATA_CMD_WRITE_SECTORS",
        "LBA28", "BlockScanMBR", "BLOCK_SECTOR_SIZE = 512",
    ))
    ext2 = require(KERNEL / "mistix_ext2.inc", (
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
        "SysShmCreate", "SysShmMap", "SysShmUnmap", "MISTIX_SYSCALL_SHM_CREATE",
    ))
    if "Reserved: array[0..6] of Byte" not in lib or "Words: array[0..5] of QWord" not in lib:
        fail("Userland IPC message ABI does not match the kernel TIPCMessage")

    init_rpc = require(ROOT / "userland/init/mistixinit.pas", (
        "if R <> MISTIX_EAGAIN then Exit", "SysCapabilityTransfer",
    ))
    vfs_retry = require(ROOT / "userland/vfs/mistixvfsd.pas", (
        "if Status = MISTIX_EAGAIN then SysYield",
        "else if IsError(Status) then SysExit(12)",
        "if Request.WordCount < 2 then Exit",
    ))
    block_retry = require(ROOT / "userland/block/mistixblockd.pas", (
        "if Status = MISTIX_EAGAIN then SysYield",
        "else if IsError(Status) then SysExit(12)",
        "if Request.WordCount < 2 then Exit",
    ))

    vfsd = require(ROOT / "userland/vfs/mistixvfsd.pas", (
        "_binary_vfs_base_img_start", "VFS_RPC_OPEN", "VFS_RPC_READ",
        "VFS_RPC_CLOSE", "ValidateArchive", "ArchiveFind", "SysIPCReceive",
    ))
    blockd = require(ROOT / "userland/block/mistixblockd.pas", (
        "BLOCK_RPC_INFO", "BLOCK_RPC_READ", "BLOCK_RPC_WRITE",
        "BLOCK_RAMDISK_BYTES", "SysCapabilityReceive", "SysIPCSend",
    ))
    init = require(ROOT / "userland/init/mistixinit.pas", (
        "TestVFSService", "TestBlockService", "SysCapabilityTransfer",
        "VFSdPath", "BlockdPath", "VFS_RPC_PING", "BLOCK_RPC_WRITE",
    ))
    shm = require(KERNEL / "mistix_shm.inc", (
        "P7_SHM_OBJECT_MAX", "P7_SHM_PAGES_MAX", "AddressSpaceMapPage",
        "ProcessCapabilityInstall", "P7ShmReleaseProcess", "PTE_USER",
    ))
    process = require(KERNEL / "mistix_process.inc", (
        "okMemoryObject", "AllowedRights", "P7ShmReleaseProcess(ProcessIndex)",
    ))
    shm = require(KERNEL / "mistix_shm.inc", (
        "P7ShmProcessHasMappings", "P7ShmReleaseProcess",
    ))
    syscall = require(KERNEL / "mistix_syscall.inc", (
        "MISTIX_ERR_BUSY", "P7ShmProcessHasMappings(CurrentProcessIndex)",
    ))
    vfsd = require(ROOT / "userland/vfs/mistixvfsd.pas", (
        "VFS_RPC_CREATE", "VFS_RPC_WRITE", "VFS_RPC_UNLINK",
        "VFS_SHARED_BASE", "TempPathAllowed", "SharedBuffer",
    ))
    blockd = require(ROOT / "userland/block/mistixblockd.pas", (
        "BLOCK_SHARED_BASE", "SysShmMap", "SharedBuffer", "BLOCK_RPC_WRITE",
    ))
    devd = require(ROOT / "userland/device/mistixdevd.pas", (
        "DEV_RPC_REGISTER", "DEV_RPC_LIST", "DEV_RPC_INFO", "DeviceRecord",
    ))
    netd = require(ROOT / "userland/network/mistixnetd.pas", (
        "NET_RPC_IFINFO", "NET_RPC_ARP_LOOKUP", "NET_RPC_UDP_ECHO",
        "BuildLoopbackFrame", "ValidateLoopbackFrame", "UDPChecksum",
    ))
    for token in ("TestDeviceManager", "TestNetworkService", "ShareMemoryWith",
                  "VFS_RPC_CREATE", "VFS_RPC_WRITE", "SysShmCreate"):
        if token not in init:
            fail(f"init test harness is missing {token}")

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
        "mistix_block.inc", "mistix_ext2.inc", "mistix_mount.inc",
        "mistix_net.inc", "mistix_shm.inc", "vfs_base.img", "vfsd.elf", "blockd.elf",
        "devd.elf", "netd.elf", "mistixdevd.pas", "mistixnetd.pas",
        "_binary_initfs_img_start", "userland/vfs/mistixvfsd.pas",
        "$(QEMU) -M pc",
    ))
    require(ROOT / "tools/mkinitfs.py", ("MFS1", "MFS1 supports at most 48 files"))
    require(ROOT / "README.md", (
        "VFS サーバー", "ATA PIO", "Ext2", "blockd", "Pascal の実コンパイル",
        "未実施", "Phase 7", "shared-memory",
    ))

    forbidden = list(ROOT.rglob("*.c")) + list(ROOT.rglob("*.h"))
    if forbidden:
        fail(f"Unexpected C source/header: {forbidden[0].relative_to(ROOT)}")

    print("[ OK ] Kernel include graph and Phase 7 subsystem ordering checked.")
    print("[ OK ] ATA PIO block layer, MBR scan, read-only Ext2 and mount metadata checked.")
    print("[ OK ] Ethernet/IPv4/UDP parser and packet self-test are present.")
    print("[ OK ] Userland VFS RPC, shared-memory block RPC and capability bootstrap checked.")
    print("[ OK ] Phase 7 per-process shared-memory mappings, transferable memory capabilities, and fork safety checked.")
    print("[ OK ] VFS tmpfs overlay, device registry, and loopback UDP service checked.")
    print("[ OK ] MFS1 build graph and embedded service image symbols checked.")
    print("[NOTE] Structural checks do not replace FPC compilation or QEMU hardware testing.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
