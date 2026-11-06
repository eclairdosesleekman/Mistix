# Mistix Phase 7 Design Notes

## Boot/service sequence

```text
UEFI -> Limine -> Mistix kernel -> init (Ring 3)
                                  |
             +--------------------+----------------------+
             |                    |                      |
             v                    v                      v
           vfsd                 blockd                  devd
             |                    |
             | MFS1 + /tmp        | volatile RAM disk
             |                    |
             +--------------------+----------------------+
                                  |
                                  v
                                netd
                                  |
                       loopback / ARP / UDP echo
```

`init` owns one 16-page shared-memory object (64 KiB), maps it at `0x500000000000`, then transfers restricted Memory Object capabilities to `vfsd`, `blockd`, and `netd`. Each service maps the same physical pages at the same user virtual address in its own PML4. IPC messages carry operation metadata; larger read/write payloads live in the shared window.

## Shared-memory ABI

Syscall IDs:

| ID | Name | Arguments |
|---:|---|---|
| 25 | `shm_create` | `RDI=page_count`, returns process Capability handle |
| 26 | `shm_map` | `RDI=handle`, `RSI=user_base`, `RDX=writable` |
| 27 | `shm_unmap` | `RDI=handle`, `RSI=user_base` |

The kernel validates the process-local Capability, object generation, requested rights, user address range and page alignment. Mapping flags always include USER and, when NX is supported, NO_EXECUTE. WRITE is included only if the caller requested a writable mapping and owns `CAP_RIGHT_WRITE`.

The current implementation uses a fixed registry: 16 objects, 16 pages per object, 64 mappings. There is no object-destroy syscall or cross-process reference count yet; the object pages remain allocated until reboot. Per-process mappings are removed on process termination. Empty page-table levels are not reclaimed by the current generic address-space layer. Because the Phase 4 COW tracker does not yet understand shared-object ownership, `fork()` returns `EBUSY` while the caller has any active shared-memory mapping; this avoids treating shared object pages as ordinary COW-owned pages.

## Capability transfer rules

- Endpoints may transfer SEND and/or RECEIVE rights.
- Memory Objects may transfer MAP, READ, WRITE and USER_MAP rights.
- The sender must have `CAP_RIGHT_TRANSFER` and every requested right.
- The receiver gets a new process-local handle with its own generation field.
- The receiver never inherits the sender's numeric handle.

## Userland VFS protocol v2

The service still has a small request endpoint and reply endpoint. The shared-memory window holds data bytes, while the IPC message carries metadata.

| Operation | Request words | Shared window |
|---|---|---|
| PING | none | unused |
| OPEN | path bytes in words 0..4 | unused |
| STAT | path bytes in words 0..4 | unused |
| CREATE | path bytes in words 0..4 | unused |
| READ | `handle`, `count` | response bytes at offset 0 |
| WRITE | `handle`, `count` | request bytes at offset 0 |
| CLOSE | `handle` | unused |
| UNLINK | path bytes in words 0..4 | unused |

MFS1 is the immutable base. `/tmp` is an eight-entry volatile upper layer with a 4096-byte maximum per file. Writes to paths outside `/tmp/` are rejected with `EROFS`. Directories are not yet enumerated by this server.

**Transition boundary:** the existing kernel `open/read/write/getdents` syscall handlers still call the Phase 5 in-kernel VFS. This phase proves that the userland `vfsd` can serve file payloads through IPC + Shared Memory; it does not yet route every POSIX-style FD operation through `vfsd`.

## Block service

`blockd` owns a 64 KiB volatile RAM disk. A client writes bytes to shared-window offset zero, then sends `(disk_offset, length)` in the RPC. For READ, the server copies disk bytes into the shared window and replies with the byte count. This is shared-memory bulk transfer, not hardware DMA. ATA PIO and the kernel Ext2 parser remain disconnected from this userland service.

## Device registry

`devd` stores up to 16 metadata records. IDs encode a slot and generation, so removing and reusing a slot does not revive stale IDs. It does not enumerate PCI or grant I/O-port/MMIO access.

## Network service

`netd` exposes a software loopback interface only. For UDP echo, it builds an Ethernet/IPv4/UDP frame in the shared region, computes the IPv4 and UDP pseudo-header checksums, validates the result, and copies the payload back as a loopback receive. ARP lookup only knows the synthetic loopback address/MAC. No physical frame is transmitted.

## Expected serial output (illustrative, not observed)

```text
[ OK ] Process-shared memory registry initialised.
[ OK ] vfsd spawned with shared-memory window.
[ OK ] VFS RPC v2 handshake passed.
[VFS] /etc/motd via shared-memory READ:
Welcome to Mistix!
[ OK ] Writable tmpfs overlay create/write/read passed.
[ OK ] blockd spawned with shared-memory bulk-transfer window.
[ OK ] Shared-memory block READ/WRITE round-trip passed.
[ OK ] Userland device-manager register/list RPC passed.
[ OK ] Loopback IPv4/UDP + ARP-cache RPC self-test passed.
```

## Security/reliability follow-up

1. Add object reference counts and destruction after all process mappings/Capabilities are gone.
2. Integrate shared mappings with `fork()`/COW ownership so shared mappings can be inherited without converting them into private COW pages.
3. Make transfer and mapping operations transactional across all pages, including page-table allocation rollback.
4. Add a proper VFS client protocol with per-client reply endpoints and request IDs, then migrate `open/read/write/getdents` off the kernel VFS.
5. Add a block-device Capability and pass it to VFS rather than exposing kernel ATA functions.
6. Port Ext2 read support into the VFS server and use the block RPC as its only I/O path.
7. Add a real virtio-net/e1000 driver, ARP packet processing, IPv4 routing and UDP sockets.
