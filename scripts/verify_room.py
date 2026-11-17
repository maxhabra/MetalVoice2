#!/usr/bin/env python3
"""Check pinned WPE reference, MIT license and independent DSP fixture hashes."""
import hashlib
import json
from pathlib import Path
root = Path(__file__).resolve().parent.parent
for directory, manifest in [('ThirdParty/NaraWPE','SOURCE_MANIFEST.json'),('Tests/Fixtures','RoomDereverb.json')]:
    folder = root / directory
    data = json.loads((folder/manifest).read_text())
    for name, expected in data['files'].items():
        actual = hashlib.sha256((folder/name).read_bytes()).hexdigest()
        if actual != expected: raise SystemExit(f'Hash mismatch: {directory}/{name}')
print('Verified pinned NARA-WPE source/license and independent room DSP fixtures')
