#!/usr/bin/env python3
"""Structural consistency checks for Mistix Phase 5 (not a Pascal compiler)."""
from __future__ import annotations
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KERNEL = ROOT / "src" / "kernel"


def fail(message: str) -> None:
    print(f"[FAIL] {message}", file=sys.stderr)
    raise SystemExit(1)


def require_tokens(path: Path, tokens: tuple[str, ...]) -> str:
    if not path.is_file():
        fail(f"Missing file: {path.relative_to(ROOT)}")
    text = path.read_text(encoding="utf-8")
    for token in tokens:
        if token not in text:
            fail(f"{path.relative_to(ROOT)} is missing {token!r}")
    return text


def main() -> int:
    kernel_main = require_tokens(ROOT / "src/kernel/mistixkernel.pas", ("mistix_vfs.inc", "mistix_process.inc", "mistix_syscall.inc"))
    includes = re.findall(r"\{\$I\s+([^}]+)\}", kernel_main, flags=re.I)
    for include in includes:
        if not (KERNEL / include.strip()).is_file():
            fail(f"Missing kernel include: {include}")
    order = ["mistix_address_space.inc", "mistix_cow.inc", "mistix_vfs.inc", "mistix_process.inc", "mistix_syscall.inc", "mistix_timer.inc", "mistix_trap.inc", "mistix_main.inc"]
    for item in order:
        if item not in includes:
            fail(f"Kernel include graph omits {item}")
    for left, right in zip(order, order[1:]):
        if includes.index(left) > includes.index(right):
            fail(f"Kernel include order must place {left} before {right}")

    all_kernel = "\n".join(p.read_text(encoding="utf-8") for p in KERNEL.glob("*.inc"))
    required_kernel_symbols = (
        "VFSInitialize", "VFSOpenUser", "VFSReadUser", "VFSWriteUser",
        "VFSPipeUser", "VFSGetDentsUser", "VFSProcessClone",
        "VFSPrepareSpawnFromParent", "VFSSelfTest", "ProcessSpawnFromPath",
        "ProcessCopyCommandLine", "SyscallDispatchFrame", "TrapDispatch",
    )
    for symbol in required_kernel_symbols:
        if not re.search(rf"\b{re.escape(symbol)}\b", all_kernel):
            fail(f"Missing kernel symbol: {symbol}")

    vfs = require_tokens(KERNEL / "mistix_vfs.inc", (
        "Ord('M')", "Ord('F')", "Ord('S')", "Ord('1')",
        "vnDirectory", "vnRegular", "vnConsole", "vnNull", "vnSerial",
        "voPipeRead", "voPipeWrite", "VFS_PIPE_CAPACITY", "VFS_DIRENT_NAME_MAX = 64",
        "VFSProcessCloseAll", "AddressSpaceCopyFrom", "AddressSpaceCopyTo",
        "VFS_ERR_AGAIN", "VFS_ERR_PIPE", "VFS_ERR_ROFS",
        "VFSGetDentsUser", "VFSChdirUser", "VFSGetCWDUser",
    ))
    if len(re.findall(r"^\s*PathPtr, DataPtr:\s*Pointer;\s*$", vfs, flags=re.M)) != 1:
        fail("InitFS parser pointer declarations are inconsistent")
    if re.search(r"\b(FillChar|Move|GetMem|FreeMem|New|Dispose|WriteLn|ReadLn)\s*\(|\b(SysUtils|Classes)\b", all_kernel, re.I):
        fail("Potential hosted RTL dependency found in kernel source")

    syscall = require_tokens(KERNEL / "mistix_syscall.inc", (
        "MISTIX_SYSCALL_OPEN = 15", "MISTIX_SYSCALL_READ = 16",
        "MISTIX_SYSCALL_CLOSE = 17", "MISTIX_SYSCALL_DUP2 = 18",
        "MISTIX_SYSCALL_PIPE = 19", "MISTIX_SYSCALL_GETDENTS = 20",
        "MISTIX_SYSCALL_CHDIR = 21", "MISTIX_SYSCALL_GETCWD = 22",
        "MISTIX_SYSCALL_SPAWN_PATH = 23", "MISTIX_SYSCALL_GETCMDLINE = 24",
        "VFSOpenUser", "VFSReadUser", "VFSWriteUser", "VFSPipeUser",
        "VFSGetDentsUser", "VFSChdirUser", "VFSGetCWDUser",
    ))

    user_files = {
        "init": ROOT / "userland/init/mistixinit.pas",
        "shell": ROOT / "userland/sh/mistixsh.pas",
        "ls": ROOT / "userland/bin/mistixls.pas",
        "cat": ROOT / "userland/bin/mistixcat.pas",
        "echo": ROOT / "userland/bin/mistixecho.pas",
    }
    user_text = {name: path.read_text(encoding="utf-8") for name, path in user_files.items()}
    for name, text in user_text.items():
        if "mistixlib.inc" not in text or "mistix_user_main" not in text:
            fail(f"{name} is not a Pascal syscall-based user program")
    for token in ("SysSpawnPath", "SysWaitPID", "MotdPath"):
        if token not in user_text["init"]:
            fail(f"Init program is missing {token}")
    for token in ("MistixShellMain", "RunPipeline", "SysPipe", "SysChdir", "SysRead"):
        if token not in user_text["shell"]:
            fail(f"Shell is missing {token}")
    for token in ("SysGetDents", "MISTIX_O_DIRECTORY"):
        if token not in user_text["ls"]:
            fail(f"ls is missing {token}")
    for token in ("SysRead", "SysWriteAll", "MISTIX_EAGAIN"):
        if token not in user_text["cat"]:
            fail(f"cat is missing {token}")
    if "SysWriteAll" not in user_text["echo"]:
        fail("echo does not handle partial writes")

    lib = require_tokens(ROOT / "userland/common/mistixlib.inc", (
        "TMistixDirent", "SysOpen", "SysRead", "SysWrite", "SysClose",
        "SysDup2", "SysPipe", "SysGetDents", "SysChdir", "SysGetCWD",
        "SysSpawnPath", "SysWriteAll",
    ))
    if "Name: array[0..63] of Char" not in lib:
        fail("User and kernel dirent ABI name field sizes differ")

    mkinitfs = require_tokens(ROOT / "tools/mkinitfs.py", ("MFS1", "--file", "MFS1 supports at most 48 files"))
    makefile = require_tokens(ROOT / "Makefile", (
        "mistix_vfs.inc", "tools/mkinitfs.py", "initfs.img",
        "_binary_initfs_img_start", "_binary_user_init_elf_start",
        "userland/sh/mistixsh.pas", "userland/bin/mistixcat.pas",
    ))
    readme = require_tokens(ROOT / "README.md", ("VFS", "InitFS", "DevFS", "Pipe", "実コンパイルと起動テストは未実施"))

    forbidden = list(ROOT.rglob("*.c")) + list(ROOT.rglob("*.h"))
    if forbidden:
        fail(f"Unexpected C source/header: {forbidden[0].relative_to(ROOT)}")

    print("[ OK ] Pascal kernel include order and Phase 5 dependency graph checked.")
    print("[ OK ] InitFS, DevFS, FD tables, shared open descriptions and pipes checked.")
    print("[ OK ] Syscall ABI 15..24 and Pascal init/shell/utilities checked.")
    print("[ OK ] MFS1 image builder, embedded-image build rules and docs checked.")
    print("[ OK ] No C source/header or hosted kernel RTL dependency found.")
    print("[NOTE] This is a structural check, not a substitute for FPC compilation or QEMU boot.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
