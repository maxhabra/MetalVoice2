#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 scripts/verify_dpdfnet.py
mkdir -p build/module-cache build/dpdfnet-tmp
model_hash="$(shasum -a 256 Resources/DPDFNet2.manifest.json | cut -d ' ' -f 1)"
if [[ -d build/DPDFNet2.mlmodelc && -f build/dpdfnet-model.stamp && "$(cat build/dpdfnet-model.stamp)" == "$model_hash" ]]; then
    exit 0
fi
swiftc -O -target arm64-apple-macosx15.0 -module-cache-path "$PWD/build/module-cache" \
    scripts/CompileCoreML.swift -o build/compile-coreml
TMPDIR="$PWD/build/dpdfnet-tmp" build/compile-coreml Resources/DPDFNet2.mlpackage build/DPDFNet2.mlmodelc
echo "$model_hash" > build/dpdfnet-model.stamp
