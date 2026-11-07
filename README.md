# Mistix 

Mistix is UNIX-like microkernel OS that is written from scratch in Pascal.
Pascal is clarify and safely than C, so Mistix use Pascal.
Mistix aim for general-purpose OS as a complete alternative to Linux and BSD, focus on readable source code and practicalness.


## Phase 11 — Userland Ext2 directory descriptors

Phase 11 connects the Phase 10 directory protocol to the existing VFS broker
for the read-only Ext2 namespace mounted at `/mnt/disk`.

- `open(path, O_DIRECTORY)` for `/mnt/disk` obtains an opaque directory
  handle from `vfsd` and stores it in a remote-directory open description.
- `getdents(fd, buffer, count)` requests at most 16 packed entries per RPC.
  The kernel validates the copied records before exposing them to userland.
- `chdir(path)` under `/mnt/disk` is confirmed by `vfsd`; only then does
  the kernel update the calling process's cwd.
- Final close, `dup2` replacement and process exit reuse the remote close
  queue. Ext2 `.` / `..` and unsafe names are filtered out.

Run `make phase11-check` for static source-contract checks.

The broker still permits one outstanding transaction. `getcwd()` continues
to read the kernel's confirmed per-process cwd cache. Ext2 directory parsing
is bounded to 128 visible entries and repeated for each batch. FPC
compilation, ISO generation and QEMU runtime testing have not been run.


## Phase 12 — Multi-request VFS broker

Phase 12 expands the kernel broker to eight outstanding transactions. Each
request owns an 8 KiB lane in the 64 KiB shared-memory region, so queued
READ/WRITE and directory operations do not overwrite another request's
arguments or transfer data. The vfsd stages one lane into the legacy shared
window while it dispatches each request, then saves read results back to that
lane.

Replies are demultiplexed by transaction ID, owner PID and server generation.
The kernel applies a five-second deadline, checked when it pumps replies on
broker retries (there is no separate watchdog thread), sends a best-effort
CANCEL notification on timeout or owner exit, and ignores late replies once
their transaction slot is retired. Cancel notification is not a preemptive abort of
a synchronous operation already executing inside vfsd; the server still
processes requests serially.

Run `make phase12-check` for static source-contract checks. FPC compilation,
ISO generation and QEMU runtime testing have not been run.

## Phase 13 — Remote and local file seeking

Phase 13 adds the lseek syscall and the SysLseek userland wrapper. Regular
files opened through the kernel VFS and remote files owned by vfsd support
SEEK_SET, SEEK_CUR, and SEEK_END. Offsets are signed 64-bit values;
negative seeks before byte zero and arithmetic overflow return EINVAL.
Pipes, streams, and directory descriptors are not seekable.

For remote files, the kernel transports the offset in the request's private
8 KiB lane and keeps the six-word IPC message layout unchanged. vfsd updates
the shared open-description position, so duplicated descriptors observe
the same cursor. Run make phase13-check for source-contract checks. Free Pascal
compilation, ISO generation and QEMU runtime tests have not been run.
