# Mistix Phase 10 — Directory handles and asynchronous VFS ABI

Phase 10 starts the next VFS migration step on top of the Phase 9 broker.
This change is additive: syscall numbers 1–31 and the six-word IPC message
layout are not redefined. The new Pascal include specifies the versioned
protocol that the kernel broker and vfsd must adopt together.

## Goals

- Opaque server-side directory handles: DIR_OPEN, DIR_READ, DIR_CLOSE.
- Move chdir and getcwd to the VFS authority.
- Correlate asynchronous requests by non-zero 64-bit RequestID and OwnerPID.
- Define cancellation as a terminal state; do not immediately reuse an ID.
- Validate directory entry name length, kind and NUL terminator.
- Keep paths absolute at the service boundary.

## ABI

The ABI is in userland/common/mistix_vfs_protocol.inc. Structures are packed
and versioned. A request contains RequestID, OwnerPID, operation, opaque handle,
bounded path and bounded shared-window offset/length. A response must echo
RequestID and OwnerPID. A mismatched response must never complete another
process's syscall.

Directory entries contain inode, size, kind, explicit NameLength and a
fixed-size NUL-terminated name. Consumers must validate both length and
terminator.

## Transaction lifecycle

FREE -> SENT -> DONE -> FREE

Cancellation is SENT -> CANCELLED -> FREE. A cancelled ID remains reserved
until the server acknowledges cancellation or the broker retires the server
generation. Request IDs are monotonically increasing; zero is reserved. On
wrap, drain all outstanding requests before restarting at one. The ABI caps
in-flight requests at 32.

## Security and correctness

1. Validate user buffers before enqueue and again before copy-out.
2. Validate shared-window offset and length with subtraction-based bounds
   checks to avoid integer overflow.
3. Match both RequestID and OwnerPID on every response.
4. Bind remote directory handles to server generation and owning process.
5. Reject dot entries, slash, embedded NUL and control bytes in names.
6. Do not hold a kernel lock while waiting for IPC.
7. A server restart invalidates handles from the previous generation.
8. Timed-out requests are cancelled; late responses are ignored.
9. getcwd reports required size and returns ERANGE without partial copy-out.
10. chdir updates per-process state only after server confirmation.

## Integration plan

Phase 10 adds the ABI and executable source-contract checks. Phase 11
routes Ext2 /mnt/disk directory open/read and chdir through the existing
single-slot broker. getcwd remains backed by the kernel's confirmed cwd cache;
MFS1 and tmpfs directory proxying, a multi-slot transaction table,
cancellation, timeouts and runtime tests remain future integration work.

## Current limitations

- Syscall 20 (getdents) and 21 (chdir) use the broker only for Ext2
  paths under /mnt/disk; syscall 22 (getcwd) reads the kernel cwd cache.
- The broker still has one outstanding transaction slot.
- No timeout worker or cancellation acknowledgement is wired into the kernel.
- MFS1/tmpfs directory proxying, Ext2 writes, UID/GID permission enforcement
  and SMP locking are not included in Phase 11.
