#!/usr/bin/env python3
"""Build Mistix's simple read-only MFS1 init filesystem image."""
from __future__ import annotations
import argparse, struct
from pathlib import Path

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--file", action="append", default=[], metavar="/PATH=HOST_FILE")
    args = parser.parse_args()
    entries: list[tuple[str, bytes]] = []
    seen: set[str] = set()
    for spec in args.file:
        if "=" not in spec:
            parser.error(f"invalid --file mapping: {spec}")
        guest, host = spec.split("=", 1)
        parts = guest.split("/")[1:] if guest.startswith("/") else []
        try:
            guest_bytes = guest.encode("ascii")
        except UnicodeEncodeError:
            parser.error(f"InitFS paths must be ASCII: {guest!r}")
        if (not guest.startswith("/") or guest == "/" or len(guest_bytes) > 127 or
                any(part in ("", ".", "..") for part in parts) or
                any(len(part) > 63 for part in parts)):
            parser.error(f"unsafe or non-canonical initfs path: {guest}")
        if guest in seen:
            parser.error(f"duplicate path: {guest}")
        if len(entries) >= 48:
            parser.error("MFS1 supports at most 48 files")
        seen.add(guest)
        entries.append((guest, Path(host).read_bytes()))
    blob = bytearray(struct.pack("<4sII", b"MFS1", 1, len(entries)))
    for guest, payload in entries:
        encoded = guest.encode("ascii")
        if len(encoded) > 127:
            parser.error(f"path too long: {guest}")
        blob += struct.pack("<HHQ", len(encoded), 0, len(payload))
        blob += encoded
        blob += payload
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    print(f"[ OK ] MFS1 image: {len(entries)} files, {len(blob)} bytes")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
