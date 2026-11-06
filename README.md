# Mistix — Phase 0 bootstrap (x86-64)

A deliberately small first boot milestone for Mistix. The kernel logic is Pascal; the only machine-specific instructions are tiny Free Pascal inline-assembler routines in the same `.pas` source file for the entry stub and x86 port I/O.

## Phase 0 acceptance criteria

When booted under QEMU with a recent Limine BIOS/UEFI hybrid ISO, the kernel should:

1. Enter at `_start` in 64-bit mode.
2. Establish a known call-stack alignment and enter Pascal `KernelMain`.
3. Initialise COM1 (16550-compatible UART) and print a Mistix banner.
4. Verify that Limine accepted protocol base revision 6.
5. Receive and print the HHDM offset and the first memory-map entries.
6. Return to the entry stub and halt with interrupts disabled.

This is **not yet a scheduler, userspace, interrupt subsystem, or memory allocator**. Phase 0 intentionally leaves interrupts disabled and does not reclaim any bootloader memory.

## Source layout

- `src/kernel/mistixkernel.pas` — Pascal kernel unit, Limine requests, COM1 output, `_start`.
- `linker.ld` — higher-half ELF64 layout required by the Limine protocol.
- `boot/limine.conf` — boot menu entry.
- `Makefile` — compile, link, verify, ISO build and QEMU run targets.

No C source, C library, libc, or FPC hosted program startup object is used by the kernel. The source includes a small amount of x86-64 inline assembly because the CPU entry point and privileged I/O instructions cannot be expressed as ordinary Pascal statements in FPC.

## Build dependencies

On an x86-64 Linux development host:

- Free Pascal Compiler (FPC), x86-64 Linux compiler target (3.2.x or newer recommended)
- GNU binutils (`ld`, `nm`, `readelf`)
- GNU Make
- Limine v12 source tree built locally
- `xorriso`
- `qemu-system-x86_64`

Limine is kept outside this repository. Clone and build the official Limine repository, then pass its path as `LIMINE_DIR`:

```sh
git clone --branch v12.x https://github.com/limine-bootloader/limine.git ../limine
make -C ../limine
```

If your Limine checkout produces binaries in a different directory, adjust `LIMINE_DIR` or the paths in the Makefile.

## Build the kernel ELF

```sh
make clean
make kernel
make verify
```

The build deliberately compiles the Pascal unit to an object (`-Cn`) and links it with `ld -nostdlib`. The Makefile checks the object for undefined symbols before linking. If FPC introduces an RTL/runtime dependency, the build stops instead of silently linking a hosted runtime.

Useful inspection commands:

```sh
readelf -h build/kernel.elf
readelf -l build/kernel.elf
nm -n build/kernel.elf | head -40
```

The entry address should be `_start` in the higher-half region beginning at `0xffffffff80000000`.

## Build and run the hybrid ISO

```sh
make iso LIMINE_DIR=../limine
make run LIMINE_DIR=../limine
```

QEMU is configured with one virtual CPU, 256 MiB RAM and COM1 connected to the terminal. Successful boot output should resemble:

```text
Mistix - Phase 0 bootstrap
Pascal freestanding kernel / x86-64
[ OK ] Limine base revision 6 accepted.
[ OK ] Limine stack-size request acknowledged.
HHDM offset: 0x...
Memory map entries: 0x...
  [0x...] base=0x... length=0x... type=0x... usable
[PASS] Phase 0 entry, Pascal code and COM1 output are alive.
```

Exact memory-map values vary by QEMU version and machine configuration.

## Important implementation notes

- The request block uses Limine's base revision 6 tag and request delimiters. The base revision tag is writable because Limine updates its third 64-bit word to zero when the requested revision is accepted.
- The memory-map response is bootloader-owned memory. This phase only reads it; it does not free or modify any entry.
- The UART wait loop is bounded so a missing or unresponsive serial device cannot hang the kernel forever.
- The linker places text, read-only data and writable data in separate ELF load segments. The kernel image is linked into Limine's required higher-half address range.
- The kernel is built without stack checking, I/O checking, range checking or overflow checking because their failure paths would require a runtime. These checks should be reintroduced selectively once a freestanding panic/check-failure path exists.

## Known limits / next work

1. Verify the exact FPC version and target backend on the intended developer machine. This environment did not have FPC or QEMU installed, so the Pascal compilation and VM boot have **not** been executed here.
2. Add a real panic path and early IDT before enabling interrupts.
3. Parse and validate the complete memory map, then implement a physical page allocator without reclaiming bootloader memory prematurely.
4. Add GDT/TSS, exception handlers, timer source and a minimal context-switch routine.
5. Add the first isolated user address space and IPC endpoint.

## License

The Mistix Phase 0 source is provided under the MIT License; see `LICENSE`.
