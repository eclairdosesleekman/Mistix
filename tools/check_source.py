#!/usr/bin/env python3
"""Structural checks for the freestanding Mistix Phase 3 Pascal sources."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KERNEL = ROOT / "src" / "kernel"
MAIN = KERNEL / "mistixkernel.pas"
MAKEFILE = ROOT / "Makefile"
USER_INIT = ROOT / "userland" / "init" / "mistixinit.pas"


def fail(message: str) -> None:
    print(f"[FAIL] {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    main_text = MAIN.read_text(encoding="utf-8")
    include_names = re.findall(r'\{\$I\s+([^}]+)\}', main_text, flags=re.IGNORECASE)
    if not include_names:
        fail("No Pascal include files found in kernel unit")
    for name in include_names:
        path = KERNEL / name.strip()
        if not path.is_file():
            fail(f"Missing include: {path.relative_to(ROOT)}")

    required = [
        "mistix_capability.inc", "mistix_ipc.inc", "mistix_notification.inc",
        "mistix_memory_object.inc", "mistix_address_space.inc",
        "mistix_process.inc", "mistix_syscall.inc",
    ]
    for name in required:
        if name not in include_names:
            fail(f"Kernel unit does not include {name}")

    joined = "\n".join(p.read_text(encoding="utf-8") for p in KERNEL.glob("*.inc"))
    for symbol in (
        "AddressSpaceCreate", "AddressSpaceLoadELF64", "AddressSpaceActivate",
        "ProcessCreateFromELF", "ProcessCapabilityLookup", "SyscallDispatch",
        "EnterUserMode", "AddressSpaceValidateUserRange", "IPCSend", "IPCReceive",
    ):
        if not re.search(rf"\b{re.escape(symbol)}\b", joined):
            fail(f"Missing Phase 3 symbol: {symbol}")

    forbidden = re.compile(
        r"\b(FillChar|Move|GetMem|FreeMem|New|Dispose|WriteLn|ReadLn)\s*\(|"
        r"\b(SysUtils|Classes)\b",
        flags=re.IGNORECASE,
    )
    match = forbidden.search(joined)
    if match:
        fail(f"Potential hosted RTL dependency found in kernel includes: {match.group(0)}")

    if not USER_INIT.is_file():
        fail("Missing Pascal ring-3 init source")
    user_text = USER_INIT.read_text(encoding="utf-8")
    for token in ("mistix_user_main", "_start", "int $80", "MistixWrite", "MistixExit"):
        if token not in user_text:
            fail(f"User init source is missing {token}")

    make_text = MAKEFILE.read_text(encoding="utf-8")
    for name in required:
        if name not in make_text:
            fail(f"Makefile dependency list omits {name}")
    for target in ("user-verify", "_binary_user_init_elf_start", "mistix_syscall_dispatch"):
        if target not in make_text:
            fail(f"Makefile/source integration is missing {target}")

    print("[ OK ] Pascal kernel include graph is complete.")
    print("[ OK ] Process address-space, ELF, capability and syscall symbols are present.")
    print("[ OK ] Ring-3 Pascal init image and int 0x80 ABI are present.")
    print("[ OK ] No obvious hosted RTL dependency found in kernel includes.")
    print("[ OK ] Makefile tracks Phase 3 kernel and user-image sources.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
