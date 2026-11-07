# Mistix Phase 14 — Build, test, and boot validation

## Goal

Phase 14 establishes a repeatable host-side validation path before the project
adds more kernel features. It does not change the kernel ABI or claim that the
current kernel is production-ready.

## Build profiles

The Makefile accepts `BUILD_TYPE=release` or `BUILD_TYPE=debug`. Their
outputs are isolated under `build/release/` and `build/debug/`, so changing
compiler diagnostics cannot silently reuse objects produced by the other
profile.

Both profiles currently retain `-O-`. Release omits debug information; debug
adds FPC debug information (`-g`) without implicitly linking the LineInfo RTL unit. Debug mode
**does not enable Pascal range or overflow checks**: the freestanding kernel
and userland do not yet provide the runtime error handlers those switches may
require. Enabling those checks is a separate task once Mistix has its own
panic/abort hooks and the resulting freestanding link has been verified.

Convenience targets:

- `make release` builds the release profile.
- `make debug` builds the debug profile.
- `make clean` removes generated build outputs.

The build remains freestanding: Pascal userland executables and the kernel are
linked with `--nostdlib`, and undefined-symbol checks remain enabled.

## Validation targets

- `make check-source` runs the structural checks for the existing kernel,
  VFS protocol and Phase 10–14 source contracts.
- `make check-python` compiles the Python tooling and runs standard-library
  `unittest` tests.
- `make test` runs both groups.
- `make all` runs tests and builds/verifies the kernel and userland.
- `make smoke` builds a hybrid Limine ISO, starts QEMU headlessly, and checks
  serial output for both the kernel's initialized marker and the init process
  reaching the shell launch path.

The MFS1 image builder is covered by integration tests for the exact binary
layout, empty payloads, duplicate paths, and unsafe/non-canonical paths.

## Continuous integration

`.github/workflows/ci.yml` runs on pushes to `main`, pull requests targeting
`main`, and manual dispatch. It installs the required host tools, downloads
the Limine v12.9.6 binary release, verifies its SHA-256 checksum, builds both
debug and release profiles, and runs the headless x86-64 QEMU boot smoke test.
Build outputs and the QEMU serial log are retained as a workflow artifact,
including when a job fails.

The Limine release is pinned to a version and checksum so CI does not silently
pick up a different bootloader build.

## Scope and known limits

- The smoke test exercises the BIOS boot path in QEMU q35 with one virtual CPU
  and 256 MiB of RAM. The ISO also contains UEFI boot files, but this phase
  does not perform an OVMF/UEFI boot test.
- The test only proves that the expected kernel and init markers appeared
  before the timeout. It is not a comprehensive userland test, a stress test,
  an SMP test, or a hardware compatibility claim.
- Static source checks prove selected source contracts, not semantic
  correctness or memory safety.
- The project still needs a fuller unit-test seam for kernel algorithms,
  fuzzing of parsers and IPC boundaries, fault injection, longer QEMU runs,
  and physical-hardware qualification.
- `.gitignore` excludes new build outputs and Python bytecode. Previously
  tracked build artifacts are removed from version control separately.

## Acceptance criteria

1. `make test` passes without FPC or QEMU.
2. `make release` builds all current kernel and userland ELFs with no
   unexpected unresolved symbols.
3. `make debug` produces a separate build with debug symbols.
4. `make smoke` produces an ISO and observes the kernel and userland boot
   markers in QEMU serial output.
5. GitHub Actions performs the same checks for each pull request to `main`.
