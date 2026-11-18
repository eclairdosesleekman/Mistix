# Phase 20 — Process/Thread separation and scheduling

This change is the first scheduler foundation slice, not completion of Phase 20.

## Implemented

- 32 priority levels (31 highest) with FIFO ordering within a priority.
- ThreadCreatePriority and ThreadSetPriority; legacy ThreadCreate remains a priority-16 wrapper.
- Dedicated idle execution context and stack. When all workers are blocked, the idle context executes STI/HLT and checks ready work after interrupts.
- Existing keyed wait/wake interface is preserved; ThreadWake wakes a blocked kernel thread by ID.
- Single-CPU only.

## Remaining Phase 20 work

The current user-process path still owns SavedFrame and KernelStackTop in TKernelProcess and is scheduled separately by ProcessScheduleFromFrame. This slice does not yet move user register context, kernel stack, TLS, scheduling state, or process thread lists into a unified Thread object. Timer preemption of kernel threads, deadline sleep/timers, user thread syscalls, join/detach/reaping, mutex/semaphore/condition/completion, priority inheritance, IRQ-safe spinlocks, lock-rank checking, and fork/exec multi-thread semantics remain outstanding.

The legacy process scheduler is intentionally unchanged until interrupt-frame ownership can be converted and tested without breaking the syscall/IRET ABI.

## Validation

make test includes source-contract checks and a host-side priority/FIFO scheduler model. These are not substitutes for FPC Debug/Release builds or QEMU boot and stress tests.
