# WebRTC acoustic echo cancellation

Pinned standalone **webrtc-audio-processing 2.1**, released 2025-01-22 by the
freedesktop maintainers, with Abseil 20240722.0 and the Meson wrap patch pinned
by that release. This is a stable standalone distribution, not the latest
Chromium/WebRTC checkout. See SOURCE_MANIFEST.json for original URLs, archive
hashes and all vendored source hashes. DSP sources are unmodified. The extra
subprojects/.gitignore ensures the extracted, patched Abseil tree is versioned.

The C++ wrapper enables **AEC3 only**, at 48 kHz, stereo playback reference and
mono microphone, with ARM NEON in the native library. AGC, noise suppression and
high-pass filtering are disabled; the existing independently controlled
processors run afterward. No neural residual echo model is included. This is
CPU SIMD, not Metal/Neural Engine inference.

`scripts/build_aec.sh` verifies sources, bootstraps pinned Meson/Ninja into the
ignored build directory when needed, and builds a static archive with the
vendored Abseil forced (no system Abseil ABI dependency or source downloads).
The generated archive already contains its internal dependencies. Do not merge
all intermediate archives again. First-time tool installation needs network;
subsequent source builds are offline. macOS deployment target: 15.0.

The separately extracted WebRTC noise suppressor uses the `mv_webrtc_ns`
namespace, via a compile definition, to avoid an ODR collision between its
minimal AudioBuffer compatibility shim and the full APM's AudioBuffer.

The wrapper consumes timestamp-aligned 480-sample frames. The host application
must supply genuine speaker playback; zeros are valid only when the playback
stream itself is silent. SetStreamDelayMs(0) describes the software alignment;
AEC3 estimates remaining acoustic/hardware echo-path delay. The measured
algorithmic delay is 430 samples (8.958 ms), tested across five impulse phases.
Additional device/queue latency is reported separately.

License notices for WebRTC, included third-party DSP code and Abseil are bundled
in LICENSE-NOTICES.txt and copied into the application.
