# Mistix Phase 8 Design Notes

## Trust boundary

```text
User processes
    |
    | VFS RPC + shared-memory data window
    v
vfsd (Ring 3)
    |
    | block RPC
    v
blockd (Ring 3, sole registered block service)
    |
    | syscall 28/29/30; read-only
    v
Kernel ATA PIO driver
    |
    v
Primary-master LBA28 disk
```

`vfsd` has no raw disk syscall. Only the bootstrapped `blockd` PID may call block info/read syscalls. The kernel's registration gate currently accepts the first PID-1 direct child that calls `block_register`; init starts blockd first. This is a bring-up restriction, not a general security design. A future version should require a one-time registration capability minted by the kernel/init and should represent the disk itself as a transferable device capability.

## Byte-range block RPC

`blockd` exposes byte offsets to `vfsd`, while ATA operates on 512-byte sectors. For a request `(offset, count)`:

1. `start_lba = offset div 512`
2. `prefix = offset mod 512`
3. `sector_count = ceil((prefix + count) / 512)`
4. read sectors into a bounded kernel/user scratch buffer
5. copy `[prefix, prefix + count)` into the shared-memory window

The service checks the request against reported disk capacity before issuing a syscall. The kernel independently verifies registered PID, LBA bounds, count (1..128 sectors), and user buffer range. A byte request can cover 129 sectors when it is 64 KiB long and unaligned; `blockd` therefore splits the operation into at most 128-sector syscall batches and assembles the result in a bounded 64 KiB + 511 byte scratch area.

There is no physical write syscall. `BLOCK_RPC_WRITE` returns `EROFS` for the ATA backend and only modifies the volatile RAM fallback.

## Ext2 mount policy

The userspace parser reads the 1024-byte superblock at filesystem offset 1024. It validates magic, block size, inode size, group geometry, feature flags, and total filesystem size against the containing partition. Journaled volumes are rejected. Only a primary MBR Linux partition (`0x83`) is considered; if no such partition is found, the whole device is treated as a raw filesystem image.

The parser supports direct and single-indirect data blocks. Directory enumeration uses a fixed 128-entry, 64-byte-name ABI; names longer than 63 bytes and directories exceeding that one-shot result capacity are rejected instead of being silently truncated. Unsupported indirect depth, journal replay, extents, GPT, and writable transactions are intentionally out of scope.

## Why regular syscalls are not yet brokered

Phase 5's kernel VFS stores file description state in each `TKernelProcess.Files` table and handles `open/read/close` synchronously. Replacing it requires a proxy file-description object, request correlation IDs, asynchronous IPC wait queues, interruption/exit cleanup, and a policy for descriptor inheritance across fork/spawn. Phase 8 keeps the existing syscall path intact while validating the userland VFS + blockd + Ext2 stack independently. This avoids claiming a full VFS migration before those lifecycle semantics exist. The legacy kernel boot path also still initializes its own read-only Ext2 mount; it is retained for compatibility and has not yet been removed.

## Next integration steps

1. Add kernel file-description type `fdProxy` with VFS endpoint capability and server-side object ID.
2. Add per-process pending RPC state and wait queues keyed by request ID.
3. Route `open/stat/getdents/read/write/close` through VFS IPC for mounts assigned to vfsd.
4. Preserve descriptor semantics for `dup2`, `fork`, `waitpid`, and process exit.
5. Only then add write support with block write capabilities, journaling/ordered metadata updates, and crash-consistency tests.
