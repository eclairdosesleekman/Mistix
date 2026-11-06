# Mistix Phase 11 — Userland Ext2 directory descriptors

## Scope

Phase 11 connects the Phase 10 directory operation identifiers to the
existing Phase 9 VFS broker for the read-only Ext2 namespace under
`/mnt/disk`. It does not replace the six-word IPC message layout or change
syscall numbers.

## Directory descriptor lifecycle

`open(path, O_DIRECTORY)` normalizes the path in the caller's process.
Paths under `/mnt/disk` use the broker; all other directory opens retain the
kernel VFS implementation. The server verifies the Ext2 inode mode and
allocates a generation-tagged open slot with source kind 3 and a private path
copy. The kernel records the returned handle in a distinct
`voRemoteDirectory` open description.

`dup2` and `fork` share the same open description and therefore the
server-side enumeration cursor. The last local reference queues the existing
broker CLOSE. The generation-bearing server handle prevents a stale FD from
closing a later allocation.

## getdents transport

The kernel caps requests at 16 entries and validates the complete user output
range before enqueue. `vfsd` re-reads the bounded Ext2 directory into its
shared window, skips `.` and `..`, and checks names for NUL, slash and
control bytes. It then compacts the requested batch to the start of the
shared window. The kernel validates the returned count, inode, type,
terminator and name bytes before one all-or-nothing copy-out.

The existing Ext2 parser is bounded to 128 visible entries per directory.
The implementation re-parses the directory for each batch; this keeps
server-side state small but is not optimized for large directories.

## chdir and cwd consistency

For paths under `/mnt/disk`, the kernel sends a normalized absolute path.
The server confirms that the target exists and is a directory. Only after
the matching broker reply does the kernel update the calling process's
`CurrentDirectory`. Relative opens then use this last confirmed path.
Paths outside the Ext2 mount continue through the kernel VFS.

`getcwd()` remains backed by the kernel's per-process cwd cache. This is
intentional: the cache is updated atomically with successful remote CHDIR,
so local and remote path normalization agree. The server has no separate
per-process cwd state.

## Protocol and limitations

DIR_OPEN, DIR_READ and CHDIR use the Phase 10 operation IDs. Directory close
reuses the already deployed Phase 9 CLOSE operation and handle format; the
standalone Phase 10 DIR_CLOSE identifier is not emitted by the kernel.
Request correlation remains the Phase 9 single-slot transaction ID and
owner PID check.

Not included:

- Multiple outstanding transactions, timeout worker, CANCEL acknowledgement,
  or SMP-safe broker locking.
- Directory enumeration for MFS1, tmpfs, or paths outside `/mnt/disk`.
- More than 128 visible entries in one Ext2 directory.
- Independent server-side cwd or a separate GETCWD RPC.
- Ext2 writes, UID/GID permission checks, or persistent metadata changes.
- FPC compilation, ISO build, or QEMU runtime testing.

## Validation

`tools/check_phase11.py` checks source contracts and operation wiring. It
is a static check and does not replace Pascal compilation or runtime tests.
