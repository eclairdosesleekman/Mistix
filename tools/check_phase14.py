#!/usr/bin/env python3
"""Static checks for the Phase 14 build and test infrastructure."""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def require(path: str, tokens: tuple[str, ...]) -> str:
    target = ROOT / path
    if not target.is_file():
        raise SystemExit(f"missing file: {path}")
    text = target.read_text(encoding="utf-8")
    missing = [token for token in tokens if token not in text]
    if missing:
        raise SystemExit(f"{path}: missing {', '.join(missing)}")
    return text


def main() -> int:
    makefile = require("Makefile", (
        "BUILD_TYPE ?= release",
        "FPC_DIAGNOSTIC_FLAGS ?= -g",
        "BUILD_DIR ?= build/$(BUILD_TYPE)",
        "check-build-tools:",
        "check-python:",
        "test: check-source check-python",
        "tools/check_phase10.py",
        "tools/check_phase14.py",
        "SMOKE_TIMEOUT ?= 45",
        "smoke: iso",
        "Kernel boot marker missing",
        "Userland init marker missing",
        "BUILD_TYPE=debug",
        "BUILD_TYPE=release",
        "mistix.iso",
    ))
    if makefile.count("tools/check_phase14.py") != 2:
        raise SystemExit("Phase 14 source check must be wired into both check-source and phase14-check.")

    require(".gitignore", (
        "/build/", "*.o", "*.ppu", "*.elf", "*.iso", "*.img",
        "__pycache__/", "*.py[cod]",
    ))
    require(".github/workflows/ci.yml", (
        "pull_request:", "push:", "workflow_dispatch:",
        "make test", "make release", "make debug", "make smoke",
        "qemu-system-x86", "limine-binary.tar.gz",
        "b38aabb042b0c0f9f562b7741be3c1204838d7bbacfa78bb411c674bd5b984ee",
        "actions/upload-artifact@v4",
    ))
    require("tools/tests/test_mkinitfs.py", (
        "test_builds_exact_mfs1_layout",
        "test_rejects_duplicate_guest_paths",
        "test_rejects_noncanonical_guest_paths",
        'b"MFS1"',
    ))
    require("docs/PHASE14-DESIGN.md", (
        "Debug and release profiles",
        "MFS1 image-builder tests",
        "headless QEMU boot smoke test",
        "does not enable Pascal range or overflow checks",
        "not a hardware compatibility claim",
    ))
    print("[ OK ] Build profiles use isolated output directories and debug symbols.")
    print("[ OK ] Phase 10–14 source checks and Python tooling tests are wired.")
    print("[ OK ] MFS1 image-builder layout and invalid-path cases have integration tests.")
    print("[ OK ] CI builds debug/release profiles and performs a headless QEMU boot smoke test.")
    print("[NOTE] Boot smoke and static checks do not replace stress, fault-injection, or real-hardware testing.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
