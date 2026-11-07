# Mistix 

Mistix is UNIX-like microkernel OS that is written by Pascal from scratch.
Pascal is easy and secure than C, Mistix aim for readable source code and security.


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
