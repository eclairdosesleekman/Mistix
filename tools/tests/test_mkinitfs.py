#!/usr/bin/env python3
"""Integration tests for the MFS1 image builder."""
from __future__ import annotations

import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILDER = ROOT / "tools" / "mkinitfs.py"


class MkInitFSTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory(prefix="mistix-mkinitfs-")
        self.root = Path(self.temp_dir.name)
        self.payload_a = self.root / "motd.txt"
        self.payload_b = self.root / "empty.bin"
        self.payload_a.write_bytes(b"Mistix test image\\n")
        self.payload_b.write_bytes(b"")

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def run_builder(self, output: Path, *mappings: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(BUILDER), str(output), *[
                argument for mapping in mappings for argument in ("--file", mapping)
            ]],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )

    def test_builds_exact_mfs1_layout(self) -> None:
        output = self.root / "nested" / "initfs.img"
        result = self.run_builder(
            output,
            f"/etc/motd={self.payload_a}",
            f"/empty={self.payload_b}",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        image = output.read_bytes()

        magic, version, entry_count = struct.unpack_from("<4sII", image, 0)
        self.assertEqual(magic, b"MFS1")
        self.assertEqual(version, 1)
        self.assertEqual(entry_count, 2)

        cursor = 12
        decoded: dict[str, bytes] = {}
        for _ in range(entry_count):
            path_length, reserved, data_length = struct.unpack_from("<HHQ", image, cursor)
            cursor += 12
            self.assertEqual(reserved, 0)
            path = image[cursor:cursor + path_length].decode("ascii")
            cursor += path_length
            data = image[cursor:cursor + data_length]
            cursor += data_length
            decoded[path] = data

        self.assertEqual(cursor, len(image), "image must not contain trailing bytes")
        self.assertEqual(decoded, {
            "/etc/motd": b"Mistix test image\\n",
            "/empty": b"",
        })

    def test_rejects_duplicate_guest_paths(self) -> None:
        output = self.root / "duplicate.img"
        result = self.run_builder(
            output,
            f"/bin/tool={self.payload_a}",
            f"/bin/tool={self.payload_b}",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("duplicate path", result.stderr)
        self.assertFalse(output.exists())

    def test_rejects_noncanonical_guest_paths(self) -> None:
        for guest_path in ("relative/path", "/etc/../passwd", "/etc//passwd", "/"):
            with self.subTest(guest_path=guest_path):
                output = self.root / "invalid.img"
                result = self.run_builder(output, f"{guest_path}={self.payload_a}")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("unsafe or non-canonical", result.stderr)
                self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
