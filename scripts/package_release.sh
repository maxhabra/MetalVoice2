#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
version="v$(plutil -extract MVReleaseVersion raw Resources/Info.plist)"
output="$PWD/build/release"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
app="$stage/MetalVoice2.app"
zip="MetalVoice2-${version}-macOS-arm64.zip"

METALVOICE_BUILD_DIR="$stage" bash scripts/build.sh release
ditto -c -k --norsrc --noextattr --keepParent "$app" "$stage/$zip"

# Verify the archived app and its first-use setup.
ditto -x -k "$stage/$zip" "$stage/extracted"
bundle="$stage/extracted/MetalVoice2.app"
binary="$bundle/Contents/MacOS/MetalVoice2"
codesign --verify --deep --strict "$bundle"
[[ "$(lipo -archs "$binary")" == arm64 ]]
[[ "v$(plutil -extract MVReleaseVersion raw "$bundle/Contents/Info.plist")" == "$version" ]]
test -r "$bundle/Contents/Resources/RVC/prepare_voice.sh"
(cd "$stage" && shasum -a 256 "$zip" > SHA256SUMS)
mkdir -p "$output"
mv "$stage/$zip" "$stage/SHA256SUMS" "$output/"
