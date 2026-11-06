# Mistix Phase 2 design notes

## Object model

Phase 2 uses three object registries (Endpoint, Notification, Memory Object). A capability entry is a revocable reference to one registry object and contains:

- object kind;
- object table index;
- object generation;
- rights bitmask;
- capability-slot generation.

The public handle encodes `slot_generation:16 | slot_plus_one:16`. This rejects closed or recycled capability slots. The registry generation independently rejects handles that refer to a destroyed and later reused object slot.

This is not yet a process security boundary: the capability table is global because there are no user processes in Phase 2. It becomes a per-process `TCapabilitySpace` once process/address-space objects are introduced.

## IPC semantics

Endpoints have an eight-entry FIFO. `IPCSend` and `IPCReceive` are blocking operations. A sender waits on a key unique to the endpoint generation when the queue is full. A receiver waits on another key when the queue is empty. Queue state is rechecked after every wakeup; waking is only a hint that the condition may now be true.

Messages contain a message id, a word count, and at most six 64-bit payload words. Message buffers are copied into the kernel queue. No pointers are transferred. Capability transfer is intentionally deferred until the process capability-space model and transfer semantics are defined.

## Notification semantics

Notifications are coalescing bitsets rather than counters. Signal ORs bits into a 64-bit pending field. Wait returns and clears only the pending bits selected by its mask. A bit signaled multiple times before it is consumed is observed once.

## Memory Object semantics

A Memory Object owns a bounded set of PMM pages. Mapping creates a PTE for one object page at a chosen page-aligned VA. Mapping records are retained so destroy can reject live mappings and unmap can verify that the VA still maps the expected object page. Multiple aliases of one object page are allowed.

Phase 2 has one active CR3 root. Its self-test maps one physical page twice in that same root and verifies coherent aliasing. Cross-process sharing requires Process objects and independent address-space roots and is a later phase.

## Scheduling and concurrency

The scheduler is cooperative and single CPU. `ThreadBlockCurrent` changes the current thread to `tsBlocked` and switches to a runnable thread. `ThreadWakeWaiters` changes matching blocked threads back to `tsRunnable`. Since IF remains clear and no SMP exists, this implementation does not use spinlocks or atomics. It must not be called from an interrupt handler.

If all threads are blocked and no thread can generate a wakeup, the current scheduler halts. An idle thread plus interrupt-driven wakeup is required before using this IPC API for services that may wait indefinitely.

## Security constraints

- Every endpoint, notification and memory-object operation validates a capability kind and rights.
- Capability restriction can only remove rights.
- Handles have generations to reject stale slot reuse.
- Object destruction revokes all capabilities referring to the destroyed object generation.
- Memory Object maps default to non-executable when NX is supported; executable mappings require an explicit execute right (not granted by the default constructor).
- This implementation is kernel-internal only. Do not expose these handles to ring 3 until per-process capability spaces and validated syscall copy-in/copy-out paths exist.
