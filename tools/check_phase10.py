#!/usr/bin/env python3
"""Static contract checks for the additive Mistix Phase 10 VFS ABI."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
abi = (ROOT / "userland/common/mistix_vfs_protocol.inc").read_text(encoding="utf-8")
doc = (ROOT / "docs/PHASE10-DESIGN.md").read_text(encoding="utf-8")

def require(text, tokens, label):
    missing = [token for token in tokens if token not in text]
    if missing:
        raise SystemExit(f"{label}: missing: {', '.join(missing)}")

require(abi, (
    "MISTIX_VFS10_ABI_VERSION = 1", "MISTIX_VFS10_DIR_OPEN",
    "MISTIX_VFS10_DIR_READ", "MISTIX_VFS10_DIR_CLOSE",
    "MISTIX_VFS10_CHDIR", "MISTIX_VFS10_GETCWD", "MISTIX_VFS10_CANCEL",
    "TMistixVFS10Request", "TMistixVFS10Response", "TMistixVFS10Dirent",
    "MistixVFS10ValidPath", "MistixVFS10ValidDirent",
    "MistixVFS10ResponseMatches",
), "Phase 10 ABI")
require(doc, ("RequestID", "OwnerPID", "CANCELLED", "server generation",
              "getdents", "getcwd", "Current limitations"), "Phase 10 design")
print("Mistix Phase 10 VFS protocol contract checks passed.")
