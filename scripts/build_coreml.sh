#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 scripts/verify_deepfilter_coreml.py
mkdir -p build/module-cache
model_hash="$(shasum -a 256 Resources/DeepFilterNet3.manifest.json | cut -d ' ' -f 1)"
if [[ -d build/DeepFilterNet3.mlmodelc && -f build/coreml-model.stamp && "$(cat build/coreml-model.stamp)" == "$model_hash" ]]; then
    exit 0
fi
swiftc -O -target arm64-apple-macosx15.0 -module-cache-path "$PWD/build/module-cache" \
    scripts/CompileCoreML.swift -o build/compile-coreml
build/compile-coreml Resources/DeepFilterNet3.mlpackage build/DeepFilterNet3.mlmodelc
echo "$model_hash" > build/coreml-model.stamp
