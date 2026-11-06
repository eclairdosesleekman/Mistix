# Mistix Phase 3 design notes

## Address-space layout

- User virtual addresses are in the lower canonical half: `[0x00400000, 0x0000800000000000)`.
- The process stack is 16 pages ending at `0x00007FFFFFF000`; the page immediately below is unmapped.
- The kernel upper PML4 entries 256..511 are copied from the Limine root and remain supervisor-only.
- User PT_LOAD pages are allocated from the PMM and mapped with PTE_USER at every level.
- The kernel's original `VMMRootPhysical` remains the bootstrap root. Process mapping functions always take an explicit root to avoid accidentally editing the wrong CR3.

## ELF policy

The loader accepts little-endian ELF64 ET_EXEC for EM_X86_64. It validates the ELF header, program-header table bounds, PT_LOAD file bounds, `filesz <= memsz`, user address range, page congruence, `p_align`, and W^X. It maps/zeroes all pages for a segment before copying file bytes through the HHDM. Dynamic linking, PT_DYNAMIC, PT_INTERP, PT_TLS, and relocations are deliberately out of scope. Ring-3 launch requires NX support so writable data and stacks can be non-executable.

## Privilege transition

The kernel GDT preserves Limine's ring-0 selectors and already contains ring-3 data selector 0x3B and code selector 0x43, plus a 64-bit TSS. Before entering user mode, Mistix refreshes TSS.RSP0 to the dedicated kernel transition stack. `iretq` enters CPL3 with the initial RIP/RSP. The IDT vector 0x80 is configured as a DPL3 interrupt gate. Hardware privilege transition switches to TSS.RSP0; the stub saves the general-purpose register frame, aligns the stack, and calls the Pascal dispatcher.

The syscall ABI is intentionally small and uses RAX for the number/result and RDI/RSI/RDX for arguments. The dispatcher writes the return value into the saved RAX slot before `iretq` restores user context.

## Capability namespaces

Each process embeds a fixed-size capability space of 64 entries. Handles encode generation in the high 16 bits and slot+1 in the low 16 bits. Slot generations are retired rather than allowed to wrap. The kernel-global Phase 2 capability API remains for the bootstrap services; user processes cannot yet operate kernel objects because capability transfer and IPC syscalls are deferred.

## Initial executable

`userland/init/mistixinit.pas` is compiled as a freestanding Pascal unit. The user linker script creates an ELF64 ET_EXEC with a fixed 0x400000 image base. The ELF is converted to a relocatable binary object by GNU ld and embedded in the kernel data segment. This avoids a second boot-time module protocol in the first Ring 3 bring-up.

## Limitations

There is one foreground user process, no timer preemption, no SMP, no dynamic ELF loader, no user fault recovery, no fork/wait, and no cross-process memory-object mapping. `yield` is currently a no-op; `exit` records status then halts the bootstrap kernel. These constraints are explicit so this phase proves the privilege boundary before adding concurrency and service IPC.
