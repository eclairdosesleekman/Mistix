# Mistix Phase 9 Design Notes

## Goal

Move ordinary regular-file `open/read/write/close` syscall handling from the kernel-resident VFS implementation to a single userland `vfsd`, without changing the Phase 8 IPC message layout or the existing application syscall numbers.

## Trust and registration

- PID 1 is the bootstrap authority.
- Only PID 1 can call syscall 31 (`vfs_register`).
- The selected `vfsd` must be PID 1's direct child and must still be alive.
- PID 1 must hold SEND on the request endpoint and RECEIVE on the reply endpoint.
- The server's shared-memory window is checked page-by-page against PID 1's mapping. All 16 pages must resolve to the same physical frames, and both mappings must be user-accessible and writable.
- Registration is idempotent for the same server and endpoints; a conflicting second registration is rejected with `EBUSY`.

This is a bootstrapping arrangement, not a general-purpose service manager or a cryptographic identity system.

## Transaction protocol

The kernel sends the following existing `TIPCMessage` fields:

| Field | Meaning |
|---|---|
| `MessageId` | broker operation identifier |
| `WordCount` | 4 |
| `Words[0]` | transaction ID |
| `Words[1]` | caller PID |
| `Words[2]` | flags for OPEN, remote handle otherwise |
| `Words[3]` | byte count for READ/WRITE |

The server replies with `MessageId` set to zero or a negative errno, `Words[0]` transaction ID, `Words[1]` caller PID, and `Words[2]` result/byte count/remote handle. The kernel requires at least three words and matches both transaction ID and caller PID.

There is one global in-flight synchronous request. Other processes see `EAGAIN` until the slot becomes available. Userland wrappers yield and retry. A retry is accepted only when the owner PID, operation, remote handle, user buffer, byte count, flags, and (for OPEN) the entire canonical path match the pending request; a changed request gets `EBUSY`. The shared buffer is therefore not overwritten by concurrent VFS requests.

Asynchronous last-reference CLOSE requests use transaction ID zero. Their replies are discarded by the broker's normal response pump and can never match a normal transaction (which starts at one).

## Data path

- OPEN path strings are canonicalized in the kernel and copied into the vfsd shared window.
- WRITE payload bytes are copied from the caller address space to a kernel scratch buffer, then to vfsd's shared window.
- READ payload bytes are returned into the shared window and copied into the caller address space only after the matching reply.
- Per-call VFS transfer is capped at `VFS_IO_MAX` (4096 bytes).

The kernel uses address-space copy helpers rather than dereferencing user virtual addresses directly.

## Remote file descriptions

The process FD table points at a kernel open-description record of kind `voRemote`. The record stores the vfsd handle and flags. Existing FD duplication and process cloning increment the shared local reference count, preserving one remote handle and shared file offset.

When the final local reference is released, `VFSReleaseDescription` queues the remote handle for asynchronous CLOSE. The same path is used by `close`, `dup2` replacement, and process-wide FD cleanup.

If vfsd dies, the broker returns `EIO` to the active caller, disables registration, and drops queued stale remote handles. The project does not yet implement service restart or FD reconstruction.

## What remains in the kernel

This phase only proxies regular-file operations. The following still use the existing kernel VFS implementation:

- `/dev/console`, `/dev/serial`, `/dev/null`
- `O_DIRECTORY` opens
- `getdents`
- `chdir` and `getcwd`
- pipe file descriptions

Therefore this is a partial VFS migration, not a claim that all VFS policy has left the kernel.

## Deliberate limits

- One global broker and one outstanding request.
- Single-CPU design; no SMP synchronization.
- No request cancellation or independent watchdog.
- Userland wrapper retry count is finite.
- MFS1 is read-only; Ext2 is read-only; `/tmp` RAM overlay is writable.
- No directory-FD proxy, symlink layer, permissions, mount namespace, or durable metadata updates.
