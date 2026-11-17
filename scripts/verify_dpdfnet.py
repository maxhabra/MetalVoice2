#!/usr/bin/env python3
"""Verify pinned upstream files and the complete streaming Core ML export."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = json.loads((root / 'ThirdParty/DPDFNet/SOURCE_MANIFEST.json').read_text())
for path, expected in source['files'].items():
    assert hashlib.sha256((root / 'ThirdParty/DPDFNet/upstream' / path).read_bytes()).hexdigest() == expected, path
model = json.loads((root / 'Resources/DPDFNet2.manifest.json').read_text())
assert model['model'] == 'dpdfnet2_48khz_hr' and model['precision'] == 'Float32' and model['fused_gru']
assert model['unrolled_gru'], 'Shipping export must use the qualified unrolled GRUs'
assert (model['sample_rate'], model['hop'], model['delay_samples'], model['state_size']) == (48000,480,2400,56436)
for path, expected in model['files'].items():
    assert hashlib.sha256((root / path).read_bytes()).hexdigest() == expected, path
fixture = json.loads((root / 'Tests/Fixtures/DPDFNet2.json').read_text())
for path, expected in fixture['files'].items():
    assert hashlib.sha256((root / 'Tests/Fixtures' / path).read_bytes()).hexdigest() == expected, path
print(f"Verified {len(source['files'])} DPDFNet upstream and {len(model['files'])} export files")
