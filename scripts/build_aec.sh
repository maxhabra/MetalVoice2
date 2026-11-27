#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 scripts/verify_aec.py
if [[ ! -x build/aec-env/bin/meson || ! -x build/aec-env/bin/ninja ]]; then
    python3 -m venv build/aec-env
    build/aec-env/bin/python -m pip install --disable-pip-version-check 'meson==1.9.1' 'ninja==1.13.0'
fi
export PATH="$PWD/build/aec-env/bin:$PATH"
export MACOSX_DEPLOYMENT_TARGET=15.0
options=(setup)
if [[ -f build/aec-native/meson-private/coredata.dat ]]; then options+=(--reconfigure); fi
CC=clang CXX=clang++ meson "${options[@]}" build/aec-native ThirdParty/WebRTCAEC/webrtc-audio-processing-2.1 \
    --buildtype=release --default-library=static --wrap-mode=nodownload --force-fallback-for=abseil-cpp \
    -Dc_args=-mmacosx-version-min=15.0 -Dcpp_args=-mmacosx-version-min=15.0
meson compile -C build/aec-native -j6
# Meson already merges the APM's internal libraries and pinned Abseil into this archive.
source_archive=build/aec-native/webrtc/modules/audio_processing/libwebrtc-audio-processing-2.a
if ! cmp -s "$source_archive" build/aec-native/libmv_webrtc_aec.a; then
    cp "$source_archive" build/aec-native/libmv_webrtc_aec.a
fi
