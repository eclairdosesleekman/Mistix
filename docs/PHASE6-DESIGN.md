# Mistix Phase 6 Design Notes

## 1. Service bootstrap over capabilities

`init` creates two endpoints per service:

- request endpoint: service receives; client sends
- reply endpoint: service sends; client receives

After `spawn_path`, `init` transfers receive-only rights for the request endpoint and send-only rights for the reply endpoint. `cap_receive()` in the child obtains those handles in transfer order. This prevents the service from consuming its own replies and keeps the RPC direction explicit.

## 2. VFS RPC

The IPC ABI has a fixed 64-byte message with six 64-bit words. The initial protocol therefore uses bounded payloads:

| Opcode | Request words | Reply |
|---|---|---|
| PING | none | version, file count |
| OPEN | packed NUL-terminated path in 40 bytes | generation-tagged open handle |
| STAT | packed path | file length |
| READ | handle, count <= 40 | count + 40 bytes |
| CLOSE | handle | status |

`vfsd` validates the complete MFS1 archive once at start-up, then bounds-checks each entry again when resolving it. Open handles contain slot + generation to reject stale handles. The service is read-only.

## 3. Block RPC prototype

`blockd` demonstrates the same service pattern with a 64 KiB in-memory disk. It reports 128 sectors of 512 bytes and supports bounded read/write windows. This is a service/ABI prototype, not a physical-disk service and not persistent storage.

## 4. Physical block path

The kernel ATA PIO path currently probes only primary master and supports LBA28. MBR entries are collected as partition records. The Ext2 reader uses a `(device, start LBA, sector count)` tuple and checks the partition bounds before every sector read.

The next implementation should move hardware access behind a capability-checked block endpoint. The current direct kernel calls are an interim bring-up layer.

## 5. Ext2 read-only subset

Supported subset:

- block sizes 1024, 2048, 4096
- ext2 superblock and group descriptors
- inode table lookup
- directory records with or without `FILETYPE`
- regular files using 12 direct block pointers
- sparse-super and large-file read-only compatible flags

Unsupported or rejected:

- journal feature (requires replay)
- unsupported incompatible features
- btree directory feature
- indirect blocks, HTree traversal, extents
- write operations

Mount metadata is separate from the Phase 5 VFS tree. `open/read` does not yet consult this mount table.

## 6. Network packet core

The parser accepts Ethernet ARP and IPv4 frames, validates IPv4 version/IHL/length/checksum, and extracts UDP ports and bounded payload metadata. The included self-test constructs a synthetic IPv4/UDP frame. This does not imply that a network interface is present or that frames can be received/transmitted.
