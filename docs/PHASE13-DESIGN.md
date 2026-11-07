# Mistix Phase 13 — File seeking

## Objective

Expose file-offset control through a new `lseek(fd, offset, whence)` syscall
(number 32) and the `SysLseek` userland wrapper. The same semantics apply to
local regular-file open descriptions and remote regular files managed by
`vfsd`.

## ABI

- `SEEK_SET = 0`: base offset is zero.
- `SEEK_CUR = 1`: base offset is the current open-description position.
- `SEEK_END = 2`: base offset is the file size.
- The offset is a signed 64-bit two's-complement value. The syscall returns
  the resulting unsigned byte position.
- Unknown `whence`, seeking before byte zero, and unsigned addition overflow
  return `EINVAL`.

The syscall consumes the normal x86-64 argument registers: RDI is the file
descriptor, RSI is the signed offset bit pattern, and RDX is `whence`.

## Remote VFS protocol

Broker message `$4D56534600000015` is reserved for SEEK and broker operation
8. The existing six-word IPC envelope is unchanged. Words 0–5 retain their
Phase 12 meanings: transaction ID, owner PID, remote handle, `whence`,
request lane, and server generation.

The signed 64-bit offset is staged in the first eight bytes of that request's
private 8 KiB lane. `vfsd` reads the offset from its staged base window,
validates the handle and `whence`, computes the target position with explicit
underflow/overflow checks, updates the server-side open slot, and returns the
new position in response word 2. No additional lane copy-back is needed.

The shared file offset belongs to the server-side open slot, so duplicated
local descriptors that reference the same remote handle share the same cursor.
The single-threaded `vfsd` serializes the update along with READ and WRITE.

## Local VFS behavior

The kernel-local VFS permits seeking regular files and updates the existing
shared `TVFSOpenDescription.Position`. This naturally preserves shared cursor
semantics across `dup2` and process-clone references. Pipes and stream devices
return `ESPIPE`; directories return `EISDIR`.

## Limits and validation

- Seeking past EOF is allowed; a subsequent read returns EOF. The existing
  write-side capacity checks still decide whether a later write can extend
  a writable temporary file.
- Remote directory descriptors are not seekable in this phase.
- The syscall is additive; existing syscall numbers and the six-word IPC
  message structure are unchanged.
- `tools/check_phase13.py` checks ABI numbering, local offset arithmetic,
  remote request staging, server-side position updates, and build wiring.
- Static source checks do not replace Free Pascal compilation, ISO generation,
  or QEMU runtime testing. Those have not been run as part of this change.
