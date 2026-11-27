#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/module-cache
export CLANG_MODULE_CACHE_PATH="$PWD/build/module-cache"
swiftc -module-cache-path build/module-cache Sources/MetalVoice2/VoiceLibrary.swift Tests/VoiceLibrary/main.swift -o build/voice-library-tests
build/voice-library-tests
voice_python="$HOME/Library/Application Support/MetalVoice2/RVC/venv/bin/python3"
if [[ -x "$voice_python" ]]; then "$voice_python" -B Tests/VoiceGate/test_gate.py; fi
python3 scripts/verify_webrtc.py
python3 scripts/verify_rnnoise.py
python3 scripts/verify_room.py
bash scripts/build_aec.sh
bash scripts/build_localvqe.sh
bash scripts/build_deepfilter.sh
bash scripts/build_coreml.sh
bash scripts/build_dpdfnet.sh
CARGO_HOME="$PWD/build/cargo-home" CARGO_TARGET_DIR="$PWD/build/deepfilter" MACOSX_DEPLOYMENT_TARGET=15.0 CFLAGS_aarch64_apple_darwin=-mmacosx-version-min=15.0 \
    cargo test --manifest-path ThirdParty/DeepFilterNet/bridge/Cargo.toml --release --locked
options=(--build-system native --disable-sandbox --cache-path "$PWD/build/swift-cache" --config-path "$PWD/build/swift-config" --security-path "$PWD/build/swift-security")
binary_dir="$(swift build "${options[@]}" -c release --show-bin-path)"
for product in transport-tests noise-tests coreml-tests dpdf-tests room-tests aec-tests localvqe-tests; do
    if [[ build/deepfilter/release/libmetalvoice_df.a -nt "$binary_dir/$product" || build/aec-native/libmv_webrtc_aec.a -nt "$binary_dir/$product" || build/localvqe-native/libmv_localvqe.a -nt "$binary_dir/$product" ]]; then
        rm -f "$binary_dir/$product"
    fi
done
swift build "${options[@]}" -c release --product transport-tests
"$binary_dir/transport-tests"
clang++ -std=c++20 -dynamiclib Tests/RouteFixture/HAL.cpp -framework AudioToolbox -framework CoreAudio \
    -framework CoreFoundation -o build/libRouteFixture.dylib
DYLD_INSERT_LIBRARIES="$PWD/build/libRouteFixture.dylib" "$binary_dir/transport-tests" --route-fixture
swiftc -module-cache-path build/module-cache -I Sources/AudioCore/include Sources/MetalVoice2/{AudioDevice,DeviceRecovery}.swift \
    Tests/DeviceRecovery/main.swift -o build/device-recovery-tests
build/device-recovery-tests
swift build "${options[@]}" -c release --product noise-tests
clang -std=c11 -dynamiclib Tests/AllocationTracker.c -o build/libAllocationTracker.dylib
DYLD_INSERT_LIBRARIES="$PWD/build/libAllocationTracker.dylib" "$binary_dir/noise-tests"
swift build "${options[@]}" -c release --product coreml-tests
coreml_args=("$PWD/build/DeepFilterNet3.mlmodelc")
if [[ -f build/deepfilter-speech.f32 ]]; then coreml_args+=("$PWD/build/deepfilter-speech.f32"); fi
"$binary_dir/coreml-tests" "${coreml_args[@]}"

swift build "${options[@]}" -c release --product dpdf-tests
TMPDIR="$PWD/build/dpdfnet-tmp" "$binary_dir/dpdf-tests" "$PWD/build/DPDFNet2.mlmodelc" "$PWD/Resources/DPDFNet2.state.f32" \
    "$PWD/Tests/Fixtures/DPDFNet2.input.f32" "$PWD/Tests/Fixtures/DPDFNet2.reference.f32" "$PWD/build/DeepFilterNet3.mlmodelc"

swift build "${options[@]}" -c release --product room-tests
TMPDIR="$PWD/build/dpdfnet-tmp" DYLD_INSERT_LIBRARIES="$PWD/build/libAllocationTracker.dylib" "$binary_dir/room-tests" \
    "$PWD/Tests/Fixtures/RoomDereverb.input.f32" "$PWD/Tests/Fixtures/RoomDereverb.reference.f32" \
    "$PWD/build/DeepFilterNet3.mlmodelc" "$PWD/build/DPDFNet2.mlmodelc" "$PWD/Resources/DPDFNet2.state.f32"

swift build "${options[@]}" -c release --product aec-tests
"$binary_dir/aec-tests"

build/localvqe-env/bin/cmake --build build/localvqe-native --target localvqe-failure-test
DYLD_INSERT_LIBRARIES="$PWD/build/libAllocationTracker.dylib" GGML_NTHREADS=2 build/localvqe-native/localvqe-failure-test Resources/localvqe-v1.3-4.8M-f32.gguf
GGML_NTHREADS=2 build/localvqe-native/localvqe-regression \
    --gguf Resources/localvqe-v1.3-4.8M-f32.gguf \
    --input ThirdParty/LocalVQE/upstream/ggml/tests/fixtures/regression_input.f32 \
    --expected ThirdParty/LocalVQE/upstream/ggml/tests/fixtures/localvqe-v1.3-4.8M-f32.out.f32
swift build "${options[@]}" -c release --product localvqe-tests
"$binary_dir/localvqe-tests" "$PWD/Resources/localvqe-v1.3-4.8M-f32.gguf"
