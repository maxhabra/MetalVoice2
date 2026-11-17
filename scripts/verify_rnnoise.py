#!/usr/bin/env python3
"""Verify the pinned native RNNoise subset and official regular model files."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1] / "ThirdParty" / "RNNoise"
manifest = json.loads((root / "SOURCE_MANIFEST.json").read_text())
for entry in manifest["files"]:
    data = (root / entry["path"]).read_bytes()
    if hashlib.sha256(data).hexdigest() != entry["sha256"]:
        raise SystemExit(f"Pinned RNNoise file changed: {entry['path']}")
    if "gitBlobSHA1" in entry:
        actual = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
        if actual != entry["gitBlobSHA1"]:
            raise SystemExit(f"RNNoise Git blob changed: {entry['path']}")
print(f"Verified {len(manifest['files'])} pinned RNNoise source/model files")
