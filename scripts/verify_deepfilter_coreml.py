#!/usr/bin/env python3
"""Verify the shipped full-precision streaming export and its weight source."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
manifest = json.loads((root / "Resources/DeepFilterNet3.manifest.json").read_text())
source = root / "ThirdParty/DeepFilterNet/upstream/models/DeepFilterNet3_onnx.tar.gz"
assert hashlib.sha256(source.read_bytes()).hexdigest() == manifest["source_sha256"], "Model source changed"
assert not manifest["fp16"] and not manifest["stateful"] and manifest["conditional"], "Unexpected model contract"
assert manifest["skip_inactive"], "Inactive decoder work must be skipped"
for relative, expected in manifest["files"].items():
    actual = hashlib.sha256((root / relative).read_bytes()).hexdigest()
    assert actual == expected, f"Core ML export changed: {relative}"
print(f"Verified {len(manifest['files'])} Core ML export files")
