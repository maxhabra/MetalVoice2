#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 scripts/verify_deepfilter.py
export CARGO_HOME="$PWD/build/cargo-home"
export CARGO_TARGET_DIR="$PWD/build/deepfilter"
export MACOSX_DEPLOYMENT_TARGET=15.0
# Pinned cc-rs/Tract assembly builds do not track the deployment-target variable.
export CFLAGS_aarch64_apple_darwin="-mmacosx-version-min=15.0"
# The lockfile pins the complete dependency graph. Build never changes it.
cargo build --manifest-path ThirdParty/DeepFilterNet/bridge/Cargo.toml --release --locked
