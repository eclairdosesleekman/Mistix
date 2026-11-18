# Mistix Phase 19 — Dynamic memory implementation notes

## Status and scope

This document records the first code landing for the Phase 19 design. It replaces the fixed 64 GiB PMM bitmaps and introduces frame references, a Buddy allocator, and a small kernel heap. It is a single-CPU implementation: allocator critical sections preserve/disable IF and do not yet provide SMP locks.

## Physical memory map

- Limine entries are range-checked for null pointers, entry-count limits, zero lengths, and base+length overflow.
- USABLE entries are aligned inward to complete 4 KiB pages. Every non-USABLE entry is treated as reserved and aligned outward.
- The ranges are sorted and coalesced; reserved intervals are subtracted from USABLE intervals, so reserved memory wins on overlap.
- PFN 0 is excluded. Physical addresses outside the x86-64 PTE address mask are clipped.
- Each resulting disjoint usable region reserves a prefix of its own pages for a `TPageFrame` array. Those metadata pages are pinned/permanent. This keeps bootstrap independent of the heap and avoids requiring metadata for holes in the physical map.
- Buddy blocks do not cross a normalized region or zone boundary.

## Buddy allocator and ownership

The allocator uses 4 KiB minimum pages and per-zone free lists for orders 0..40. DMA32 means physical addresses below 4 GiB; normal allocations prefer the Normal zone and may fall back to DMA32, while `afDMA32` is strict. Free-time coalescing is limited to a buddy in the same metadata region and zone.

`PMMAllocatePage` is retained as an Order-0 compatibility wrapper. `PMMFreePage` is a reference-dropping wrapper, not an unconditional free. The base frame of a Buddy allocation stores its order and initial refcount; non-head frames are not independently referenceable until an Order-0 block is allocated from them.

`PageGet`/ `PagePut` are the common Order-0 ownership operations. COW now uses these frame references rather than a second fixed reference table. Shared-memory objects retain their original allocation reference; every process mapping obtains one additional reference and releases it on unmap or process teardown.

## Kernel heap

The slab size classes are 16, 32, 64, 128, 256, 512, 1024, and 2048 bytes. Slab headers and allocation bitmaps live inside the backing page. An empty slab is unlinked and returned to PMM immediately.

Allocations larger than 2048 bytes, or allocations requesting alignment above 16 bytes, use a Buddy-backed page block with an in-band validation header. `KAllocPages`/`KFreePages` expose page blocks and currently require a power-of-two page count so the exact Buddy order is unambiguous. `KAlloc(0)`, zero alignment, and non-power-of-two alignment are rejected. `KAllocZero` explicitly clears the requested object bytes.

## Tests and current limitations

- Boot-time self-tests exercise an Order-1 allocation/free, refcount get/put, double-put rejection, slab allocation/reuse/free, zero allocation, aligned large allocation, and free-page accounting.
- `tools/check_phase19.py` checks integration contracts; `tools/tests/test_phase19_memory.py` tests a host-side memory-map subtraction and Buddy model with randomized allocate/free sequences.
- The existing page-table and COW/SHM callers use the new page API, while the rest of the kernel continues through compatible Order-0 wrappers.
- This phase does not add swap, NUMA, huge-page allocation, demand paging, SMP locking, or a general Pascal RTL allocator. Heap allocation from interrupt context is not a supported contract; future scheduler/locking work must define that boundary explicitly.
