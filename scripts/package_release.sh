#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
version="v$(plutil -extract MVReleaseVersion raw Resources/Info.plist)"
output="$PWD/build/release"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
app="$stage/MetalVoice2.app"
zip="MetalVoice2-${version}-macOS-arm64.zip"
pkg="MetalVoice2-VoiceRuntime-${version}-macOS-arm64.pkg"

METALVOICE_BUILD_DIR="$stage" bash scripts/build.sh release
bash scripts/setup_voice_runtime.sh "$app" "$stage/$pkg"
ditto -c -k --norsrc --noextattr --keepParent "$app" "$stage/$zip"

# Verify the actual downloads, including the worker seal in the archived executable.
ditto -x -k "$stage/$zip" "$stage/extracted"
bundle="$stage/extracted/MetalVoice2.app"
binary="$bundle/Contents/MacOS/MetalVoice2"
codesign --verify --deep --strict "$bundle"
[[ "$(lipo -archs "$binary")" == arm64 ]]
[[ "v$(plutil -extract MVReleaseVersion raw "$bundle/Contents/Info.plist")" == "$version" ]]
pkgutil --expand-full "$stage/$pkg" "$stage/installer"
python3 - "$binary" "$stage/seal" <<'PY'
import pathlib
import subprocess
import sys

# otool prints the arm64 section as little-endian 32-bit words after each address.
rows = subprocess.check_output(["otool", "-X", "-s", "__TEXT", "__mv_rvcseal", sys.argv[1]], text=True)
seal = b"".join(bytes.fromhex(word)[::-1] for row in rows.splitlines() for word in row.split()[1:])
if len(seal) != 64:
    raise ValueError("Missing voice worker seal")
pathlib.Path(sys.argv[2]).write_bytes(seal)
PY
cat > "$stage/main.swift" <<'SWIFT'
import Foundation
try VoiceRuntime.authenticatePayload(at: URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true))
SWIFT
swiftc -module-cache-path build/module-cache Sources/MetalVoice2/VoiceRuntime.swift "$stage/main.swift" \
    -Xlinker -sectcreate -Xlinker __TEXT -Xlinker __mv_rvcseal -Xlinker "$stage/seal" -o "$stage/check-runtime"
"$stage/check-runtime" "$stage/installer/Payload"

(cd "$stage" && shasum -a 256 "$zip" "$pkg" > SHA256SUMS)
mkdir -p "$output"
mv "$stage/$zip" "$stage/$pkg" "$stage/SHA256SUMS" "$output/"
