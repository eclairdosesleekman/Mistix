#!/usr/bin/env python3
"""Structural integration checks for the Phase 19 memory-management landing."""
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
    memory = require(KERNEL / "mistix_memory.inc", (
        "{$I mistix_memory_types.inc}", "{$I mistix_buddy.inc}",
        "{$I mistix_heap.inc}",
        "PMMBuddySelfTest", "KernelHeapSelfTest", "KernelHeapInitialize",
        "PMMAllocPages(0, [afZero, afPageTable], PhysicalAddress)",
    ))
    require(KERNEL / "mistix_memory_types.inc", (
        "TPageFrame = record", "RefCount: LongWord", "OwnerPFN: QWord",
        "PMM_FRAME_FLAG_HEAP_BIG", "afKernelHeapPages",
        "TSlabHeader = packed record",
    ))
    buddy = require(KERNEL / "mistix_buddy.inc", (
        "PMMNormalizeMemoryMap", "PMMSortAndMergeRanges",
        "PMM_MAX_ORDER", "mzDMA32", "mzNormal",
        "function PMMAllocPages", "function PMMFreePages",
        "function PageGet", "function PagePut", "function PagePin",
        "function PageUnpin", "PMMListRemove", "PMMBuddyValidate",
        "function PMMGetManagedPages",
    ))
    heap = require(KERNEL / "mistix_heap.inc", (
        "KHeapSlabAllocate", "KHeapSlabFree",
        "function KAlloc(Size: QWord; Alignment: QWord)",
        "function KAllocZero", "function KFree(P: Pointer)",
        "function KAllocPages", "function KFreePages",
        "KernelHeapSelfTest",
    ))
    types = read(KERNEL / "mistix_types.inc")
    for old in ("PMM_PAGE_COUNT", "PMM_BITMAP_BYTES"):
        if old in types or old in memory or old in buddy:
            fail(f"Fixed-bitmap PMM constant remains: {old}")

    address_space = require(KERNEL / "mistix_address_space.inc", (
        "PMMAllocPages(0, [afZero, afPageTable], NewRoot)",
        "PMMAllocPages(0, [afZero], PhysicalAddress)",
    ))
    cow = require(KERNEL / "mistix_cow.inc", (
        "function COWAcquireSharedPage", "Result := PageGet(PhysicalAddress)",
        "function COWReleasePage", "Result := PagePut(PhysicalAddress)",
        "PageReferenceCount(PhysicalAddress)",
    ))
    if "COWPageReferences" in cow or "COW_MAX_SHARED_PAGES" in cow:
        fail("COW still maintains a second fixed physical-page reference table")

    shm = require(KERNEL / "mistix_shm.inc", (
        "if not PageGet(Physical) then Break;",
        "if not PagePut(ExistingPhysical) then",
        "if not PagePut(Physical) then",
        "Process teardown could not release shared-page reference",
    ))
    if shm.count("PagePut(") < 4:
        fail("SHM map rollback, unmap and process teardown must drop frame references")

    makefile = require(ROOT / "Makefile", (
        "$(SRC_DIR)/mistix_memory_types.inc",
        "$(SRC_DIR)/mistix_buddy.inc", "$(SRC_DIR)/mistix_heap.inc",
        "tools/check_phase19.py", "phase19-check:",
    ))
    require(ROOT / "docs/PHASE19-DESIGN.md", (
        "reserved intervals", "DMA32", "PageGet", "KAllocPages",
        "power-of-two page count", "randomized allocate/free",
    ))
    for text, label in ((buddy, "Buddy PMM"), (heap, "kernel heap")):
        if re.search(r"\b(FillChar|Move|GetMem|FreeMem|New|Dispose|WriteLn|ReadLn)\s*\(|\b(SysUtils|Classes)\b", text, re.I):
            fail(f"{label} introduces a hosted RTL dependency")

    print("[ OK ] Fixed 64 GiB PMM bitmaps were removed.")
    print("[ OK ] Normalized memory ranges, DMA32/Normal Buddy and frame refs are wired.")
    print("[ OK ] Slab caches and page-backed KAlloc/KFree APIs are present.")
    print("[ OK ] Page tables, COW, and SHM use the Phase 19 allocation/ownership APIs.")
    print("[ OK ] Phase 19 build and documentation contracts are registered.")
    print("[NOTE] These checks are structural; FPC and QEMU tests are still required.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
