#!/usr/bin/env python3
"""Structural checks for Mistix Phase 4 freestanding Pascal sources."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KERNEL = ROOT / "src" / "kernel"
MAIN = KERNEL / "mistixkernel.pas"
MAKEFILE = ROOT / "Makefile"
USER_INIT = ROOT / "userland" / "init" / "mistixinit.pas"
USER_WORKER = ROOT / "userland" / "worker" / "mistixworker.pas"


def fail(message: str) -> None:
    print(f"[FAIL] {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    main_text = MAIN.read_text(encoding="utf-8")
    include_names = re.findall(r"\{\$I\s+([^}]+)\}", main_text, flags=re.IGNORECASE)
    if not include_names:
        fail("No Pascal include files found in kernel unit")
    for name in include_names:
        path = KERNEL / name.strip()
        if not path.is_file():
            fail(f"Missing include: {path.relative_to(ROOT)}")

    required = [
        "mistix_capability.inc", "mistix_ipc.inc", "mistix_notification.inc",
        "mistix_memory_object.inc", "mistix_address_space.inc", "mistix_cow.inc",
        "mistix_process.inc", "mistix_syscall.inc", "mistix_timer.inc",
        "mistix_trap.inc",
    ]
    for name in required:
        if name not in include_names:
            fail(f"Kernel unit does not include {name}")
    if include_names.index("mistix_address_space.inc") > include_names.index("mistix_cow.inc"):
        fail("COW implementation must follow the address-space implementation")
    if include_names.index("mistix_process.inc") > include_names.index("mistix_syscall.inc"):
        fail("Process manager must be included before syscall dispatch")
    if include_names.index("mistix_trap.inc") < include_names.index("mistix_timer.inc"):
        fail("Trap dispatcher must follow the timer implementation")

    joined = "\n".join(p.read_text(encoding="utf-8") for p in KERNEL.glob("*.inc"))
    for symbol in (
        "ProcessScheduleFromFrame", "ProcessForkCurrent", "ProcessSpawnFromUser",
        "ProcessWaitCurrent", "ProcessCapabilityTransfer", "ProcessIPCReceive",
        "SyscallDispatchFrame", "TrapDispatch", "TimerInitialize",
        "COWForkAddressSpace", "COWHandlePageFault", "AddressSpaceCopyFrom",
        "AddressSpaceCopyTo", "ProcessRestoreFrame",
    ):
        if not re.search(rf"\b{re.escape(symbol)}\b", joined):
            fail(f"Missing Phase 4 symbol: {symbol}")

    if "call mistix_trap_dispatch" not in (KERNEL / "mistix_cpu.inc").read_text(encoding="utf-8"):
        fail("Common ISR does not return through the unified trap dispatcher")
    if "PTE_COW" not in (KERNEL / "mistix_types.inc").read_text(encoding="utf-8"):
        fail("Missing software COW page-table marker")
    cow_text = (KERNEL / "mistix_cow.inc").read_text(encoding="utf-8")
    if "PTE_ADDRESS_MASK) or PTE_PRESENT or" not in cow_text:
        fail("COW copied page must be remapped as a present PTE")
    if "CAP_RIGHT_TRANSFER" not in (KERNEL / "mistix_capability.inc").read_text(encoding="utf-8"):
        fail("Missing transferable capability right")

    forbidden = re.compile(
        r"\b(FillChar|Move|GetMem|FreeMem|New|Dispose|WriteLn|ReadLn)\s*\(|"
        r"\b(SysUtils|Classes)\b",
        flags=re.IGNORECASE,
    )
    match = forbidden.search(joined)
    if match:
        fail(f"Potential hosted RTL dependency found in kernel includes: {match.group(0)}")

    if not USER_INIT.is_file() or not USER_WORKER.is_file():
        fail("Missing Pascal init or worker source")
    init_text = USER_INIT.read_text(encoding="utf-8")
    worker_text = USER_WORKER.read_text(encoding="utf-8")
    for token in (
        "mistix_user_main", "_start", "int $80", "MistixFork", "MistixSpawn",
        "MistixWaitPID", "MistixEndpointCreate", "MistixCapabilityTransfer",
        "_binary_worker_elf_start", "_binary_worker_elf_end", "ForkMarker",
        "$AABBCCDDEEFF0011", "$5566778899AABBCC",
    ):
        if token not in init_text:
            fail(f"User init source is missing {token}")
    for token in (
        "mistix_worker_main", "_start", "MistixCapabilityReceive", "MistixIPCSend",
        "EAGAIN_NEGATIVE", "MistixYield",
    ):
        if token not in worker_text:
            fail(f"User worker source is missing {token}")
    if "WriteBytes(" in init_text:
        fail("User init contains an undefined WriteBytes helper")
    if "Frame^.SavedRAX := 0;" not in (KERNEL / "mistix_syscall.inc").read_text(encoding="utf-8"):
        fail("yield syscall must return zero before switching processes")

    make_text = MAKEFILE.read_text(encoding="utf-8")
    for name in required:
        if name not in make_text:
            fail(f"Makefile dependency list omits {name}")
    for token in (
        "worker-verify", "user-verify", "_binary_user_init_elf_start",
        "mistix_trap_dispatch", "mistix_timer_isr", "mistix_process_restore_frame",
        "grep -v '_binary_worker_elf_'", "grep -v '_binary_user_init_elf_'",
    ):
        if token not in make_text:
            fail(f"Makefile/source integration is missing {token}")

    forbidden_c = list(ROOT.rglob("*.c")) + list(ROOT.rglob("*.h"))
    if forbidden_c:
        fail(f"Unexpected C source/header file in Pascal-only project: {forbidden_c[0]}")

    print("[ OK ] Pascal kernel include graph and dependency order are complete.")
    print("[ OK ] Preemptive frame scheduler, process lifecycle and syscall symbols are present.")
    print("[ OK ] COW mappings, page-fault resolution and copy-in/copy-out are present.")
    print("[ OK ] Pascal init/worker programs cover fork, spawn, wait and user IPC.")
    print("[ OK ] No hosted RTL dependency or C source/header file found.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
