# Mistix Phase 12 — Multi-request VFS broker

## Objective

Phase 12 replaces the Phase 9/11 singleton broker request record with a
bounded table of eight outstanding transactions. It keeps the existing
six-word IPC message ABI and existing syscall numbers.

## Request table

Each table entry records the operation, caller process index and PID,
monotonic transaction ID, VFS server generation, user buffer metadata,
result/payload, and deadline. A process retry matches its own non-cancelled
entry and must present identical operation arguments and path. A changed
request is rejected with `EBUSY`. If all eight entries are occupied, a new
caller receives `EAGAIN`.

## Shared-memory isolation

The existing 64 KiB shared region is split into eight 8 KiB request lanes.
Each request carries its lane index in IPC word 4 and the registered server
generation in word 5. The kernel stages path and write input into that lane
and later copies read/directory output from the same lane.

The vfsd remains a single-threaded service. For each incoming broker request,
it copies the selected lane into the legacy base window, dispatches the
operation (including any existing blockd traffic), and copies read output
back to the selected lane. This avoids sharing the base window between
multiple *waiting* requests while preserving the existing blockd ABI.
It does not execute VFS operations in parallel.

## Reply correlation and server generations

A normal response carries transaction ID, owner PID, payload and server
generation in words 0–3. The kernel finds the matching table entry and
requires both owner PID and server generation to match. Transaction IDs are
monotonic and are not reused during normal operation. A replacement server
receives a new non-zero generation, so replies from an older registration
cannot complete a new request.

## Timeout and cancellation

The kernel uses PIT ticks at 100 Hz and assigns a 500-tick (five-second)
deadline to each transaction. An expired request returns `ETIMEDOUT` to its
caller and sends a Phase 10 CANCEL notification. Cancelled entries are kept
briefly as tombstones so a late successful OPEN can have its remote handle
closed and a late response cannot be mistaken for a new transaction. The
tombstone is retired after a matching response/acknowledgement or a bounded
grace period.

CANCEL is best-effort, not a preemptive abort. Because vfsd processes requests
synchronously and serially, a request already being dispatched may complete
before the server receives its CANCEL notification. In that race, the kernel
still ignores the late response (and closes a late OPEN handle when observed).

## Limits

- Eight outstanding requests globally; one outstanding call per process.
- vfsd handles requests sequentially; the table enables concurrency while
  callers wait, not parallel execution inside the filesystem service.
- The shared-memory lane size is 8 KiB; individual file I/O remains capped at
  4 KiB and directory reads remain capped at 16 entries.
- CANCEL cannot interrupt a synchronous Ext2/blockd operation already in
  progress. The caller is bounded by the timeout, while server work may
  finish later.
- A timed-out request whose response is never delivered can retain its
  tombstone only until the grace period expires; a late response after that
  point is ignored.
- vfsd restart still requires a fresh registration; automatic service
  supervision and remote FD re-open are outside this phase.
- FPC compilation, ISO build and QEMU runtime testing have not been run.

## Validation

`tools/check_phase12.py` verifies source-level contracts for table bounds,
lane isolation, correlation, timeout and cancellation wiring. It is a static
check and is not a substitute for Pascal compilation or runtime tests.
