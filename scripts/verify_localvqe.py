#!/usr/bin/env python3
"""Verify vendored LocalVQE / GGML and the published joint v1.3 weights."""
import hashlib
import json
from pathlib import Path
repo = Path(__file__).resolve().parent.parent
root = repo / 'ThirdParty/LocalVQE'
data = json.loads((root / 'SOURCE_MANIFEST.json').read_text())
for name, expected in data['files'].items():
    path = root / name
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        raise SystemExit(f'LocalVQE source mismatch: {name}')
model = data['model']
path = repo / model['path']
if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != model['sha256']:
    raise SystemExit(f'LocalVQE model missing or modified: {path}')
print(f"Verified LocalVQE joint v1.3 model and {len(data['files'])} pinned source files")
