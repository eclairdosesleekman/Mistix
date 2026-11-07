# Mistix Phase 18 — User-mode page-fault isolation

## Objective

Phase 17 validates checked Pascal range/overflow failures and native x86
divide-error delivery as child-process exits. Phase 18 extends that coverage
to the user-mode page-table permissions enforced by the x86-64 MMU.

The tests are deliberately issued from separate child processes. A faulting
child must be terminated and reaped by PID 1; it must not take down the kernel,
the init process, or the following tests.

## Fault cases

The Debug-only `/bin/runtimefault` probe now supports four page-fault modes:

| Mode | Deliberate operation | Expected #PF error code |
| --- | --- | ---: |
| `pr` | Read an unmapped user virtual address | `0x4` |
| `pw` | Write an unmapped user virtual address | `0x6` |
| `pt` | Write to the probe's read-only executable text | `0x7` |
| `px` | Execute a `RET` byte stored on the NX user stack | `0x15` |

The test address for the unmapped read/write pair is in the canonical lower
half, below `USER_ADDRESS_LIMIT`, and outside the ELF image, user stack, and
Mistix's default shared-memory mapping.

The page-fault error bits used by the tests are the architectural x86 values:
present/protection violation (P, bit 0), write access (W/R, bit 1), user access
(U/S, bit 2), and instruction fetch (I/D, bit 4). Thus the expected errors are:

- `0x4`: non-present user read;
- `0x6`: non-present user write;
- `0x7`: present, user-mode write-protection violation;
- `0x15`: present, user-mode instruction fetch from an NX page.

The ELF loader maps executable text without `PTE_WRITABLE`, maps non-executable
segments with `PTE_NO_EXECUTE`, and rejects writable-plus-executable segments.
The user stack is writable and marked NX when NX support is available. ELF
loading already requires NX support, so the stack-execute case is not silently
skipped on processors without NX.

## Process isolation and status ABI

For user-mode vector 14, the trap dispatcher records the exit status as:

```
0x8000000000000000 | (14 << 32) | (page_fault_error_code & 0xffffffff)
```

PID 1 spawns each case, waits for the exact child PID, and checks the full
64-bit status. The expected statuses are:

| Case | Expected child status |
| --- | ---: |
| Unmapped read | `0x8000000E00000004` |
| Unmapped write | `0x8000000E00000006` |
| Write to executable text | `0x8000000E00000007` |
| Execute from NX stack | `0x8000000E00000015` |

A child that returns normally, reports a different fault vector/error code,
cannot be waited for, or is not reaped makes the Debug init test fail. The
existing Phase 17 runtime-check cases remain in the same suite and retain
their own pass marker.

These target pages are ordinary user mappings, not COW mappings. The trap
dispatcher still gives the existing COW handler first opportunity to resolve
a genuine copy-on-write fault; the four Phase 18 permission/missing-page cases
must fall through to user-process termination.

## Validation

- `make phase18-check` performs host-only source-contract checks for all four
  fault modes, expected error-code mappings, the trap exit-status encoding,
  and the Debug smoke requirement.
- `make test` runs the new checker along with all earlier source checks and
  Python tests.
- `make smoke BUILD_TYPE=debug` builds and boots the Debug image under QEMU
  and requires both the Phase 17 runtime-fault marker and Phase 18 page-fault
  marker.
- Release does not trigger deliberate page faults.

The README's existing `FPC 実コンパイル` source-check token is retained;
source-contract checks, actual FPC compilation, and QEMU runtime evidence are
separate validation levels.

## Scope and limitations

- These are four deterministic user-mode probes, not fuzzing. This is not
  exhaustive MMU testing or exhaustive page-table coverage.
- They validate the user-process exception path and parent wait/reap behavior;
  kernel-mode page faults remain fatal.
- They do not test SMP/TLB shootdowns, demand paging, stack growth, guard-page
  growth, all ELF layouts, or every x86 page-fault flag.
- A passing QEMU smoke test is not a hardware qualification or a general
  memory-safety proof.

## Acceptance criteria

1. The four page-fault modes compile into the freestanding test ELF.
2. Each mode is reaped by PID 1 with its exact vector-14/error-code status.
3. The Phase 18 Debug smoke marker is required by the Makefile.
4. The previous Phase 17 runtime-check cases and Release boot path remain wired.
