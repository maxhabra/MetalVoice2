#!/bin/bash
# Optional local CPU inference runtime for the v2 preview.
set -euo pipefail
runtime="$HOME/Library/Application Support/MetalVoice2/RVC/venv"
python3 -m venv "$runtime"
"$runtime/bin/python3" -m pip install --only-binary=:all: 'numpy==1.26.4' 'torch==2.7.0' 'onnxruntime==1.19.2' 'faiss-cpu==1.8.0.post1'
