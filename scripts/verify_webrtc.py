#!/usr/bin/env python3
"""Check that the pinned WebRTC source subset remains byte-for-byte upstream."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1] / "ThirdParty" / "WebRTC"
manifest = json.loads((root / "SOURCE_MANIFEST.json").read_text())
for entry in manifest["files"]:
    data = (root / entry["path"]).read_bytes()
    actual = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
    if actual != entry["gitBlobSHA1"]:
        raise SystemExit(f"Upstream source changed: {entry['path']}")
print(f"Verified {len(manifest['files'])} pinned WebRTC source files")
