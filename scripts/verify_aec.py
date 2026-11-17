#!/usr/bin/env python3
"""Verify the pinned standalone WebRTC APM and patched Abseil source tree."""
import hashlib
import json
from pathlib import Path
root = Path(__file__).resolve().parent.parent / 'ThirdParty/WebRTCAEC'
data = json.loads((root/'SOURCE_MANIFEST.json').read_text())
for name, expected in data['files'].items():
    path = root/name
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        raise SystemExit(f'AEC source mismatch: {name}')
print(f"Verified WebRTC APM 2.1 / Abseil 20240722.0: {len(data['files'])} pinned source files")
