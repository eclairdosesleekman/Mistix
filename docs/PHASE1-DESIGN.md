# Mistix Phase 1 design notes

## Bootstrap contract

- Limine protocol base revision: 6.
- Requested paging mode: x86-64 four-level paging, with min=max=preferred=4-level.
- HHDM and memory-map pointers are the virtual pointers returned by Limine; no fixed HHDM constant is assumed.
- The Limine entry selectors are retained: code `0x28`, data `0x30`.

## Physical allocator policy

The used-page bitmap begins fully allocated. Only complete 4 KiB pages wholly contained in Limine `USABLE` ranges are cleared. Page frame zero is always reserved. A second bitmap records frames allocated by this PMM instance, so reserved frames and double frees are rejected. The fixed bitmaps track 64 GiB of physical address space; pages outside this range are unavailable to the allocator.

## Virtual memory policy

The VMM walks and extends Limine's existing four-level hierarchy. It allocates page-table frames from PMM and accesses them through HHDM. It supports 4 KiB leaf mappings, and the translation helper understands existing 2 MiB and 1 GiB huge mappings. The allocator does not yet reclaim empty page-table pages.

## CPU descriptor policy

The GDT reproduces Limine's initial descriptor slots and adds ring-3 code/data slots plus a 64-bit TSS descriptor. `RSP0` points to a dedicated static transition stack. IST1 points to a dedicated double-fault stack. Ring-3 transitions and per-thread kernel-stack switching are not part of this phase.

## Thread policy

A small static thread table and statically provisioned stacks are used to validate context switching without adding a heap. The scheduler is cooperative, single-CPU, and round-robin. Timer IRQs remain masked and IF remains clear.
