#!/usr/bin/env python3
"""Verify FPC's generated x86-64 runtime-check symbols without linking."""
from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tools" / "tests" / "fpc_runtime_probe.pas"
EXPECTED = {"FPC_RANGEERROR", "FPC_OVERFLOW", "FPC_DIVBYZERO"}


def run(command: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command, cwd=cwd, text=True, capture_output=True, check=False
    )


def main() -> int:
    fpc_command = shlex.split(os.environ.get("FPC", "fpc"))
    nm_command = shlex.split(os.environ.get("NM", "nm"))
    if not fpc_command or shutil.which(fpc_command[0]) is None:
        print("FPC runtime ABI check requires Free Pascal (set FPC=...)", file=sys.stderr)
        return 2
    if not nm_command or shutil.which(nm_command[0]) is None:
        print("FPC runtime ABI check requires GNU nm (set NM=...)", file=sys.stderr)
        return 2
    if not PROBE.is_file():
        print(f"missing probe source: {PROBE}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="mistix-fpc-runtime-") as temporary:
        work = Path(temporary)
        compile_command = fpc_command + [
            "-Tlinux", "-Px86_64", "-Cn", "-O-", "-Cr", "-Co",
            "-dMISTIX_DEBUG_CHECKS", "-FU.", str(PROBE),
        ]
        compiled = run(compile_command, work)
        if compiled.returncode != 0:
            print("FPC runtime probe compilation failed:", file=sys.stderr)
            print(compiled.stdout, file=sys.stderr)
            print(compiled.stderr, file=sys.stderr)
            return compiled.returncode or 1

        objects = sorted(work.glob("*.o"))
        if len(objects) != 1:
            print(
                f"expected one probe object in temporary directory, found {len(objects)}",
                file=sys.stderr,
            )
            print(compiled.stdout, file=sys.stderr)
            return 1

        inspected = run(nm_command + ["-u", str(objects[0])], work)
        if inspected.returncode != 0:
            print("nm could not inspect the FPC probe object:", file=sys.stderr)
            print(inspected.stderr, file=sys.stderr)
            return inspected.returncode or 1

        symbols = set()
        for line in inspected.stdout.splitlines():
            parts = line.split()
            if parts:
                symbols.add(parts[-1].lstrip("_").upper())
        missing = sorted(EXPECTED - symbols)
        if missing:
            print("FPC did not emit the expected freestanding runtime hooks:", file=sys.stderr)
            print("missing: " + ", ".join(missing), file=sys.stderr)
            print("undefined symbols emitted by the probe:", file=sys.stderr)
            print(inspected.stdout, file=sys.stderr)
            return 1

        for symbol in sorted(EXPECTED):
            print(f"[ OK ] Compiler emitted {symbol}.")
        print("[ OK ] FPC x86-64 checked-code runtime ABI matches Mistix hooks.")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
