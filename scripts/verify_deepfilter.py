#!/usr/bin/env python3
"""Verify the pinned, unmodified libDF sources and DFN3 model archive."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1] / "ThirdParty" / "DeepFilterNet"
manifest = json.loads((root / "SOURCE_MANIFEST.json").read_text())
for entry in manifest["files"]:
    data = (root / entry["path"]).read_bytes()
    if hashlib.sha256(data).hexdigest() != entry["sha256"]:
        raise SystemExit(f"Pinned DeepFilterNet file changed: {entry['path']}")
    actual = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
    if actual != entry["gitBlobSHA1"]:
        raise SystemExit(f"DeepFilterNet Git blob changed: {entry['path']}")
print(f"Verified {len(manifest['files'])} pinned DeepFilterNet source/model files")
