#!/usr/bin/env python3
"""Verify FPC x86-64 checked-code hooks and native signed-division codegen."""
from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tools" / "tests" / "fpc_runtime_probe.pas"
EXPECTED_HOOKS = {"fpc_rangeerror", "FPC_OVERFLOW"}
SIGNED_DIVISION = re.compile(r"\bidiv(?:[bwlq])?\b", re.IGNORECASE)


def run(command: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command, cwd=cwd, text=True, capture_output=True, check=False
    )


def main() -> int:
    fpc_command = shlex.split(os.environ.get("FPC", "fpc"))
    nm_command = shlex.split(os.environ.get("NM", "nm"))
    objdump_command = shlex.split(os.environ.get("OBJDUMP", "objdump"))
    for label, command in (
        ("Free Pascal Compiler", fpc_command),
        ("GNU nm", nm_command),
        ("GNU objdump", objdump_command),
    ):
        if not command or shutil.which(command[0]) is None:
            print(f"FPC runtime ABI check requires {label} (set its environment variable)", file=sys.stderr)
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
                symbols.add(parts[-1].lstrip("_"))
        missing = sorted(EXPECTED_HOOKS - symbols)
        if missing:
            print("FPC did not emit the expected freestanding runtime hooks:", file=sys.stderr)
            print("missing: " + ", ".join(missing), file=sys.stderr)
            print("undefined symbols emitted by the probe:", file=sys.stderr)
            print(inspected.stdout, file=sys.stderr)
            return 1

        disassembled = run(objdump_command + ["-d", str(objects[0])], work)
        if disassembled.returncode != 0:
            print("objdump could not disassemble the FPC probe object:", file=sys.stderr)
            print(disassembled.stderr, file=sys.stderr)
            return disassembled.returncode or 1
        if not SIGNED_DIVISION.search(disassembled.stdout):
            print("FPC did not emit a native signed IDIV instruction for the division probe.", file=sys.stderr)
            print(disassembled.stdout, file=sys.stderr)
            return 1

        for symbol in sorted(EXPECTED_HOOKS):
            print(f"[ OK ] Compiler emitted {symbol}.")
        print("[ OK ] Compiler emitted native signed IDIV; divide-by-zero raises x86 #DE.")
        print("[ OK ] FPC x86-64 checked-code ABI matches Mistix handlers and trap path.")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
