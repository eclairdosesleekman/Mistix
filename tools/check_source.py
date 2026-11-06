#!/usr/bin/env python3
"""Lightweight structural checks for the freestanding Mistix Pascal sources."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KERNEL = ROOT / "src" / "kernel"
MAIN = KERNEL / "mistixkernel.pas"
MAKEFILE = ROOT / "Makefile"


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
        "mistix_capability.inc",
        "mistix_ipc.inc",
        "mistix_notification.inc",
        "mistix_memory_object.inc",
    ]
    for name in required:
        if name not in include_names:
            fail(f"Kernel unit does not include {name}")

    joined = "\n".join(p.read_text(encoding="utf-8") for p in KERNEL.glob("*.inc"))
    for symbol in (
        "CapabilityRestrict",
        "CapabilityRevokeObject",
        "IPCSend",
        "IPCReceive",
        "ThreadBlockCurrent",
        "ThreadWakeWaiters",
        "NotificationSignal",
        "NotificationWait",
        "MemoryObjectMapPage",
        "MemoryObjectUnmapPage",
        "MemoryObjectDestroy",
    ):
        if not re.search(rf"\b{re.escape(symbol)}\b", joined):
            fail(f"Missing required Phase 2 symbol: {symbol}")

    forbidden = re.compile(
        r"\b(FillChar|Move|GetMem|FreeMem|New|Dispose|WriteLn|ReadLn)\s*\(|"
        r"\b(SysUtils|Classes)\b",
        flags=re.IGNORECASE,
    )
    match = forbidden.search(joined)
    if match:
        fail(f"Potential hosted RTL dependency found: {match.group(0)}")

    make_text = MAKEFILE.read_text(encoding="utf-8")
    for name in required:
        if name not in make_text:
            fail(f"Makefile dependency list omits {name}")

    print("[ OK ] Pascal include graph is complete.")
    print("[ OK ] Phase 2 IPC/capability/notification/memory-object symbols are present.")
    print("[ OK ] No obvious hosted RTL dependency found in kernel includes.")
    print("[ OK ] Makefile tracks all Phase 2 source files.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
