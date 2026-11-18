# Mistix Phase 1 design notes

## Bootstrap contract

- Limine protocol base revision: 6.
- Requested paging mode: x86-64 four-level paging, with min=max=preferred=4-level.
- HHDM and memory-map pointers are the virtual pointers returned by Limine; no fixed HHDM constant is assumed.
- The Limine entry selectors are retained: code `0x28`, data `0x30`.

## Physical allocator policy (superseded by Phase 19)

The original bring-up implementation used two fixed bitmaps and tracked 64 GiB. Phase 19 replaces that policy with normalized firmware memory ranges, per-region page-frame metadata, DMA32/Normal zones, a Buddy allocator, and page reference counts. The bitmap description is retained here only as historical Phase 1 context; see [Phase 19 memory design](PHASE19-DESIGN.md) for the current implementation.

## Virtual memory policy

The VMM walks and extends Limine's existing four-level hierarchy. It allocates page-table frames from the Buddy PMM and accesses them through HHDM. It supports 4 KiB leaf mappings, and the translation helper understands existing 2 MiB and 1 GiB huge mappings. Empty page-table reclamation remains part of the later address-space lifecycle work.

## CPU descriptor policy

The GDT reproduces Limine's initial descriptor slots and adds ring-3 code/data slots plus a 64-bit TSS descriptor. `RSP0` points to a dedicated static transition stack. IST1 points to a dedicated double-fault stack. Ring-3 transitions and per-thread kernel-stack switching are not part of this phase.

## Thread policy

A small static thread table and statically provisioned stacks are used to validate context switching without adding a heap. The scheduler is cooperative, single-CPU, and round-robin. Timer IRQs remain masked and IF remains clear.
