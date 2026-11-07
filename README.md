# Mistix

**A Pascal-first, UNIX-like microkernel operating system for x86-64.**

Mistix is an experimental operating-system project built from scratch with Free Pascal. The kernel, initialization process, shell, utilities, and service processes are written in Object Pascal. Small inline x86-64 assembly blocks are used where the architecture requires precise control, such as entry points, interrupt handling, context transitions, and system-call stubs.

The project explores how Pascal's explicit types, structured records, and readable syntax can be used for low-level systems programming without depending on a hosted C runtime. The goal is a small, understandable system with a clear user/kernel boundary and userland services communicating through capabilities, IPC, and shared memory.

> **Project status: experimental / bring-up.** Mistix is not a production operating system, is not POSIX-complete, and is not yet a replacement for Linux or the BSDs. Interfaces and implementation details are still evolving.

## Current implementation

The latest source and design milestone in the repository is **Phase 13**. This is a description of what is present in the source tree, not a claim that every subsystem has been validated on hardware or in an emulator.

### Kernel and execution environment

- x86-64 kernel booting through the Limine boot protocol, requesting four-level paging.
- High-half kernel layout and separate lower-half user address spaces.
- Physical-page allocation and virtual-memory mapping, with user-range validation and address-space copy helpers.
- Ring 3 execution, a 64-bit TSS, interrupt/exception entry paths, and an x86-64 system-call ABI.
- ELF64 static executable loading with segment-bound checks and W^X policy; user stacks include a guard page.
- Process creation, path-based spawning, waiting, termination, and `fork()` with copy-on-write page sharing.
- Per-process capability spaces with generation-tagged handles and rights checks.
- IPC endpoints, bounded message queues, notifications, memory objects, capability transfer, and process-shared memory mappings.
- Legacy 8259 PIC / 8254 PIT timer support at 100 Hz. The current design is single-CPU; SMP support is not implemented.

### System calls

The current syscall interface is an intentionally small, project-specific ABI. It is not a promise of complete POSIX compatibility.

| Number | System call | Number | System call |
| ---: | --- | ---: | --- |
| 1 | `write` | 17 | `close` |
| 2 | `exit` | 18 | `dup2` |
| 3 | `getpid` | 19 | `pipe` |
| 4 | `yield` | 20 | `getdents` |
| 5 | `fork` | 21 | `chdir` |
| 6 | `spawn` | 22 | `getcwd` |
| 7 | `waitpid` | 23 | `spawn_path` |
| 8 | `endpoint_create` | 24 | `getcmdline` |
| 9 | `ipc_send` | 25 | `shm_create` |
| 10 | `ipc_receive` | 26 | `shm_map` |
| 11 | `cap_transfer` | 27 | `shm_unmap` |
| 12 | `cap_receive` | 28 | `block_register` |
| 13 | `cap_close` | 29 | `block_info` |
| 14 | `kill` | 30 | `block_read` |
| 15 | `open` | 31 | `vfs_register` |
| 16 | `read` | 32 | `lseek` |

The syscall wrappers used by Pascal userland programs are in `userland/common/mistixlib.inc`.

### Userland and services

The initial user process is `init` (PID 1). It starts the service processes, performs bring-up checks, and launches the interactive shell.

| Program / service | Responsibility |
| --- | --- |
| `init` | Boot-time service startup, capability distribution, IPC setup, and smoke-test routines. |
| `sh` | Basic interactive shell with `help`, `cd`, `pwd`, and `exit` built-ins; supports launching external commands and a simple single-pipe pipeline. |
| `ls`, `cat`, `echo` | Small standalone Pascal command-line utilities. |
| `vfsd` | Userland filesystem service; serves the immutable MFS1 base image, a writable volatile `/tmp` overlay, and read-only Ext2 data. |
| `blockd` | Block I/O service. It mediates bounded requests to the kernel's restricted ATA read path and also has a volatile RAM-disk backend. |
| `devd` | Userland device metadata registry. It does not enumerate PCI devices or drive hardware directly. |
| `netd` | Software loopback network service with ARP lookup and IPv4/UDP echo tests. It is not a physical network driver. |

Services use capability-protected IPC endpoints. Shared memory carries bulk data, while IPC messages carry operation metadata and handles.

### Filesystems and storage

- **MFS1** is the immutable base filesystem image embedded into the system.
- **`/tmp`** is a small writable in-memory overlay. The current VFS service defines eight temporary-file slots and a 4 KiB maximum per temporary file; the `O_CREAT` path is covered by the init create/write/read round-trip. Its contents are volatile.
- **Ext2** is supported as a read-only subset and is exposed under **`/mnt/disk`** when a supported partition is found. The userland Ext2 client performs filesystem reads through `vfsd` and `blockd`.
- The hardware block path currently targets the primary-master ATA PIO device using LBA28 and 512-byte sectors. MBR partition discovery is implemented; this is not a general storage-driver stack.
- Physical-disk writes are deliberately unavailable through the current block syscall interface. Writable filesystem behavior is limited to volatile storage such as `/tmp`.

The Ext2 implementation is intentionally bounded. Journal replay, writable Ext2 transactions, extents, HTree directories, GPT discovery, and broad compatibility with arbitrary Ext2 feature combinations are outside the current scope. See the phase design notes for the exact accepted subset.

### VFS migration status

Mistix began with kernel-resident VFS handling and progressively moved regular-file operations into userland. **Mistix Phase 9** introduced `vfs_register()` and the first regular-file `open/read/write/close` syscall broker. The initial broker admitted **1件の VFS transaction** at a time; later phases extended that design.

Current behavior is mixed by design:

- Regular-file `open`, `read`, `write`, `close`, and `lseek` operations can be brokered to `vfsd`.
- Ext2 directory opens, `getdents()`, and `chdir` for paths under `/mnt/disk` are routed through the userland VFS service.
- `getcwd()` still reads the kernel's per-process current-directory cache.
- Device nodes, pipes, and directory operations outside the supported Ext2 path retain kernel-side handling.

#### Phase 11 — Userland Ext2 directory descriptors

Phase 11 added remote directory descriptors for the read-only Ext2 namespace under `/mnt/disk`. Directory enumeration is bounded (up to 16 entries per request, with a 128-entry visible-directory limit in the current Ext2 client). The kernel validates returned entries before copying them to user memory. The current-directory cache remains kernel-owned and is updated after a successful remote `chdir`.

See [docs/PHASE11-DESIGN.md](docs/PHASE11-DESIGN.md) and run `make phase11-check` for the source-contract check.

#### Phase 12 — Multi-request VFS broker

Phase 12 replaced the original single-request broker record with a bounded table for **eight outstanding transactions**. The 64 KiB shared region is divided into eight private 8 KiB request lanes. Replies are correlated by transaction ID, owner PID, and VFS server generation.

The broker assigns a **five-second deadline** and sends best-effort cancellation notifications. Cancellation cannot interrupt a synchronous filesystem operation that the single-threaded `vfsd` is already processing; the broker has **no separate watchdog thread**. The request table allows multiple callers to wait safely, but it does not make filesystem operations execute in parallel.

See [docs/PHASE12-DESIGN.md](docs/PHASE12-DESIGN.md) and run `make phase12-check`.

#### Phase 13 — Remote and local file seeking

Phase 13 adds syscall 32, `lseek(fd, offset, whence)`, and the `SysLseek` Pascal wrapper. Both kernel-local and remote regular-file descriptions support `SEEK_SET`, `SEEK_CUR`, and `SEEK_END`. The signed 64-bit offset is staged in the request's private 8 KiB lane for remote seeks, and the server-side open description owns the shared cursor.

Pipes and stream devices are not seekable; remote directory descriptors are also not seekable in this phase.

See [docs/PHASE13-DESIGN.md](docs/PHASE13-DESIGN.md) and run `make phase13-check`.

## Phase 14 — Build and test infrastructure

Phase 14 adds repeatable host-side validation without changing the kernel ABI:

- Separate `build/release/` and `build/debug/` output trees.
- Debug symbols in the debug profile; both profiles keep `-O-` until optimization is measured.
- Static Phase 10–14 source checks and Python tooling tests.
- MFS1 image-builder integration tests for binary layout, empty files, duplicate paths, and non-canonical paths.
- A headless QEMU boot smoke test that checks kernel and init serial markers.
- GitHub Actions on pull requests and pushes to `main`, with a pinned Limine v12.9.6 release and retained build logs.

Debug mode uses FPC debug information (`-g`) only. It avoids `-gl`, which implicitly links the LineInfo RTL unit, and does not enable Pascal range or overflow checks because the freestanding programs do not yet supply the runtime error handlers those switches may need.

Run the host-side checks and profiles with:

```sh
make test
make release
make debug
```

With Limine v12 available at `../limine`, `xorriso`, QEMU, and GNU `timeout` installed, run the headless boot check with:

```sh
make smoke
```

See [docs/PHASE14-DESIGN.md](docs/PHASE14-DESIGN.md) for the exact validation scope and limitations. The smoke test is a BIOS/QEMU bring-up check, not a complete userland test, SMP test, UEFI qualification, or real-hardware certification.

## Networking

The current network implementation is a protocol/parser and software-loopback experiment:

- Ethernet frame parsing for ARP and IPv4.
- IPv4 header validation and checksum handling.
- UDP loopback echo through `netd`, with synthetic frame construction and validation.
- A small ARP cache for the loopback address.

There is no physical NIC driver, packet transmission/reception path, TCP stack, external IPv4 routing, or general socket API yet. Do not interpret the loopback self-test as proof of external network connectivity.

## Building

### Prerequisites

- Free Pascal Compiler (FPC) with the x86-64 target used by the Makefile.
- GNU binutils, including `ld`, `nm`, and `readelf`.
- Python 3.
- `xorriso` for ISO creation.
- `qemu-system-x86_64` for emulator runs.
- A built Limine v12 tree for bootable ISO creation. By default, the Makefile looks for it at `../limine`.

The `check-tools` target checks for the tools above and the expected Limine files.

### Build the kernel and userland

```sh
make check-tools
make
```

The default `make` target runs the structural source checks, builds the freestanding Pascal userland executables and filesystem images, compiles the kernel object, links the kernel ELF, and verifies key ELF symbols and unresolved-symbol constraints.

### Build and run in QEMU

```sh
make iso
make run
```

To use a Limine tree in another location:

```sh
make iso LIMINE_DIR=/path/to/limine
make run LIMINE_DIR=/path/to/limine
```

The current QEMU run target uses a q35 machine with 256 MiB of memory and one virtual CPU. Serial output is sent to the terminal.

For a paused QEMU session with the GDB stub enabled:

```sh
make run-debug
```

The debug target starts QEMU with `-s -S`.

### Source-contract checks

```sh
make check-source
make phase10-check
make phase11-check
make phase12-check
make phase13-check
make phase14-check
make test
```

These Python checks verify selected source structure, ABI constants, and wiring between the kernel and userland services. They are **static checks**, not runtime tests.

The Phase 11–13 design notes explicitly mark **FPC 実コンパイル**, ISO generation, and QEMU boot/runtime validation as **未確認** for those milestones. A passing source-contract check does not prove that the kernel compiles, the ISO boots, or a service works under load. Re-run the full build and emulator tests when validating a new revision.

## Repository layout

```text
Mistix/
├── boot/                   Limine boot configuration
├── docs/                   Phase design notes and message of the day
├── src/kernel/             Pascal kernel unit and subsystem includes
├── tools/                  Python source checks and image-generation helpers
├── userland/
│   ├── bin/                ls, cat, and echo
│   ├── block/              blockd
│   ├── common/              syscall wrappers and shared VFS protocol
│   ├── device/             devd
│   ├── init/               PID 1
│   ├── network/            netd
│   ├── sh/                 interactive shell
│   └── vfs/                vfsd and Ext2 client
├── Makefile
├── linker.ld
└── LICENSE
```

## Design notes

The design notes are the best place to find the detailed invariants and limitations for each subsystem:

- [Phase 1 — Boot and memory foundations](docs/PHASE1-DESIGN.md)
- [Phase 2 — Capabilities, IPC, notifications, and memory objects](docs/PHASE2-DESIGN.md)
- [Phase 3 — User address spaces and ELF loading](docs/PHASE3-DESIGN.md)
- [Phase 5 — VFS and file-descriptor model](docs/PHASE5-DESIGN.md)
- [Phase 6 — ATA, Ext2, and network foundations](docs/PHASE6-DESIGN.md)
- [Phase 7 — Shared memory and userland services](docs/PHASE7-DESIGN.md)
- [Phase 8 — Block-service boundary and Ext2 integration](docs/PHASE8-DESIGN.md)
- [Phase 9 — Userland VFS syscall broker](docs/PHASE9-DESIGN.md)
- [Phase 10 — Directory ABI design](docs/PHASE10-DESIGN.md)
- [Phase 11 — Ext2 directory descriptors](docs/PHASE11-DESIGN.md)
- [Phase 12 — Multi-request VFS broker](docs/PHASE12-DESIGN.md)
- [Phase 13 — File seeking](docs/PHASE13-DESIGN.md)
- [Phase 14 — Build, test, and boot validation](docs/PHASE14-DESIGN.md)

Some legacy serial-output text still uses a Phase 9 label; the source and design notes describe later Phase 11–14 work. Treat the design notes and current source as the implementation reference rather than those older display strings.

## License

Mistix is distributed under the [MIT License](LICENSE).
