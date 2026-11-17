#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
configuration="${1:-release}"
if [[ "$configuration" != "release" && "$configuration" != "debug" ]]; then
    echo "Usage: $0 [release|debug]" >&2
    exit 2
fi
python3 scripts/verify_webrtc.py
python3 scripts/verify_rnnoise.py
python3 scripts/verify_room.py
bash scripts/build_aec.sh
bash scripts/build_localvqe.sh
bash scripts/build_deepfilter.sh
bash scripts/build_coreml.sh
bash scripts/build_dpdfnet.sh
mkdir -p build/module-cache
export CLANG_MODULE_CACHE_PATH="$PWD/build/module-cache"
options=(--build-system native --disable-sandbox --cache-path "$PWD/build/swift-cache" --config-path "$PWD/build/swift-config" --security-path "$PWD/build/swift-security")
binary_dir="$(swift build "${options[@]}" -c "$configuration" --show-bin-path)"
# SwiftPM does not track an externally built -l library as a file dependency.
# Removing only this generated output forces a relink after Rust/model changes.
if [[ build/deepfilter/release/libmetalvoice_df.a -nt "$binary_dir/MetalVoice2" || build/aec-native/libmv_webrtc_aec.a -nt "$binary_dir/MetalVoice2" || build/localvqe-native/libmv_localvqe.a -nt "$binary_dir/MetalVoice2" ]]; then
    rm -f "$binary_dir/MetalVoice2"
fi
swift build "${options[@]}" -c "$configuration" --product MetalVoice2
destination="$PWD/build/MetalVoice2.app"
# Sign outside a synced Documents folder. File providers can restore FinderInfo
# immediately after xattr removes it, causing codesign to reject a fresh build.
staging_dir="$(mktemp -d "${TMPDIR:-/tmp}/metalvoice2-build.XXXXXX")"
trap 'rm -rf "$staging_dir"' EXIT
bundle="$staging_dir/MetalVoice2.app"
mkdir -p "$bundle/Contents/MacOS"
cp -X "$binary_dir/MetalVoice2" "$bundle/Contents/MacOS/MetalVoice2"
cp Resources/Info.plist "$bundle/Contents/Info.plist"
mkdir -p "$bundle/Contents/Resources/Licenses/LocalVQE"
cp -X Resources/localvqe-v1.3-4.8M-f32.gguf "$bundle/Contents/Resources/"
cp -X ThirdParty/LocalVQE/upstream/LICENSE ThirdParty/LocalVQE/README.md ThirdParty/LocalVQE/SOURCE_MANIFEST.json "$bundle/Contents/Resources/Licenses/LocalVQE/"
cp -X ThirdParty/LocalVQE/upstream/ggml/vendor/ggml/LICENSE "$bundle/Contents/Resources/Licenses/LocalVQE/GGML-LICENSE"
mkdir -p "$bundle/Contents/Resources/Licenses/WebRTCAEC"
cp -X ThirdParty/WebRTCAEC/LICENSE-NOTICES.txt ThirdParty/WebRTCAEC/README.md ThirdParty/WebRTCAEC/SOURCE_MANIFEST.json "$bundle/Contents/Resources/Licenses/WebRTCAEC/"
mkdir -p "$bundle/Contents/Resources/Licenses/NaraWPE"
cp -X ThirdParty/NaraWPE/LICENSE ThirdParty/NaraWPE/README.md ThirdParty/NaraWPE/SOURCE_MANIFEST.json "$bundle/Contents/Resources/Licenses/NaraWPE/"
mkdir -p "$bundle/Contents/Resources/Licenses/WebRTC"
cp ThirdParty/WebRTC/upstream/LICENSE ThirdParty/WebRTC/upstream/PATENTS ThirdParty/WebRTC/upstream/AUTHORS "$bundle/Contents/Resources/Licenses/WebRTC/"
cp ThirdParty/WebRTC/upstream/common_audio/third_party/ooura/LICENSE "$bundle/Contents/Resources/Licenses/WebRTC/Ooura-LICENSE"
mkdir -p "$bundle/Contents/Resources/Licenses/RNNoise"
cp ThirdParty/RNNoise/upstream/COPYING ThirdParty/RNNoise/upstream/AUTHORS \
    ThirdParty/RNNoise/README.md ThirdParty/RNNoise/LICENSE-NOTICES.txt \
    ThirdParty/RNNoise/SOURCE_MANIFEST.json "$bundle/Contents/Resources/Licenses/RNNoise/"
mkdir -p "$bundle/Contents/Resources/Licenses/DeepFilterNet"
ditto --norsrc --noextattr build/DeepFilterNet3.mlmodelc "$bundle/Contents/Resources/DeepFilterNet3.mlmodelc"
cp -X Resources/DeepFilterNet3.manifest.json "$bundle/Contents/Resources/Licenses/DeepFilterNet/"
ditto --norsrc --noextattr build/DPDFNet2.mlmodelc "$bundle/Contents/Resources/DPDFNet2.mlmodelc"
cp -X Resources/DPDFNet2.state.f32 "$bundle/Contents/Resources/"
mkdir -p "$bundle/Contents/Resources/Licenses/DPDFNet"
cp -X ThirdParty/DPDFNet/upstream/LICENSE ThirdParty/DPDFNet/SOURCE_MANIFEST.json \
    ThirdParty/DPDFNet/README.md Resources/DPDFNet2.manifest.json "$bundle/Contents/Resources/Licenses/DPDFNet/"
cp ThirdParty/DeepFilterNet/upstream/LICENSE-MIT ThirdParty/DeepFilterNet/upstream/LICENSE-APACHE \
    ThirdParty/DeepFilterNet/README.md ThirdParty/DeepFilterNet/SOURCE_MANIFEST.json \
    ThirdParty/DeepFilterNet/DEPENDENCIES.json ThirdParty/DeepFilterNet/DEPENDENCY-NOTICES.txt \
    ThirdParty/DeepFilterNet/RUST-STDLIB-NOTICES.html ThirdParty/DeepFilterNet/bridge/Cargo.lock \
    "$bundle/Contents/Resources/Licenses/DeepFilterNet/"
# Finder may decorate an opened generated bundle. Remove only signing-incompatible metadata.
xattr -dr com.apple.FinderInfo "$bundle" 2>/dev/null || true
xattr -dr com.apple.ResourceFork "$bundle" 2>/dev/null || true
codesign --force --sign - "$bundle"
codesign --verify --strict "$bundle"
mkdir -p "$destination/Contents/MacOS"
# Replace the executable atomically, keeping an already-running process intact.
cp -X "$bundle/Contents/MacOS/MetalVoice2" "$destination/Contents/MacOS/MetalVoice2.next"
mv -f "$destination/Contents/MacOS/MetalVoice2.next" "$destination/Contents/MacOS/MetalVoice2"
cp -X "$bundle/Contents/Info.plist" "$destination/Contents/Info.plist"
ditto --norsrc --noextattr "$bundle/Contents/Resources" "$destination/Contents/Resources"
ditto --norsrc --noextattr "$bundle/Contents/_CodeSignature" "$destination/Contents/_CodeSignature"
codesign --verify "$destination"
echo "Built $destination"
echo "Run: open '$destination' --args --show"
