# WebRTC noise suppression subset

Upstream: [webrtc-mirror/webrtc](https://github.com/webrtc-mirror/webrtc), pinned to commit `3710345e71ad3a7c1f4ed4efef152fd4fc24869a` (January 13, 2026), retrieved November 17, 2026. [SOURCE_MANIFEST.json](SOURCE_MANIFEST.json) records the Git blob SHA-1 of every vendored upstream file. `scripts/verify_webrtc.py` checks those hashes. All 39 files under `upstream/` are unmodified.

The static library contains the upstream `NoiseSuppressor`, its supporting estimators, Ooura FFT, `ThreeBandFilterBank`, `ArrayView`, and type traits. It does not contain the browser/network/video stack, echo cancellation, AGC, a high-pass filter, or the full AudioProcessing API.

`wrapper.cpp` integrates these components for one mono, 48 kHz stream in exact 480-sample blocks:

1. Convert normalized float samples to the FloatS16 scale used inside WebRTC.
2. Split into three bands with the upstream analysis filter.
3. Run `NoiseSuppressor::Analyze` and `Process`.
4. Merge the bands with the upstream synthesis filter and return normalized floats.

Two clearly separated compatibility headers keep this extraction small:

- `compat/modules/audio_processing/audio_buffer.h` implements only the mono split-band storage/accessors consumed by this suppressor. It is not upstream's general-purpose AudioBuffer and does not perform resampling or downmixing.
- `compat/rtc_base/checks.h` maps the vendored sources' debug-only `RTC_DCHECK*` calls to standard assertions. There are no production `RTC_CHECK` calls in this subset. This avoids pulling in Abseil logging. Changes adding such dependencies must be reviewed rather than silently disabling checks.

These are integration adaptations, not replacements for the suppression equations, window, FFT, or filter coefficients. The assembled full-band path is therefore not a claim of byte-identical browser `getUserMedia` behavior: browser configuration and other enabled stages can differ.

This subset is currently the only WebRTC code linked into the app. Before adding a full WebRTC/APM library, isolate this target's C++ namespace and FFT symbols or replace the compatibility buffer with the full upstream implementation; two different `webrtc::AudioBuffer` definitions must not coexist in the same binary.

Levels map directly to upstream `NsConfig::SuppressionLevel`: Low = k6dB, Moderate = k12dB, High = k18dB, Very high = k21dB. These names express upstream target settings, not a guarantee of measured attenuation on every sound.

The frame hop is 10 ms. The assembled path's dominant impulse peak is delayed by **334 samples at 48 kHz (6.9583 ms)**: 288 samples from NS overlap plus 46 from the analysis/synthesis bank. Tests check all four levels and several impulse positions. This does not include frame collection, worker scheduling, transport queues, or devices. The native engine adjusts sample-origin timestamps by this amount before latency measurement.

Construction/reset allocates; `mv_ns_process` is single-threaded and tested for zero C++ heap allocations. Worker scheduling and device behavior require live benchmarks separately.

## Notices

WebRTC's [BSD license](upstream/LICENSE), [patent grant](upstream/PATENTS), and [authors](upstream/AUTHORS) are retained. The embedded FFT also carries [Ooura's license](upstream/common_audio/third_party/ooura/LICENSE) and its upstream [README](upstream/common_audio/third_party/ooura/README.chromium). The app bundle includes these license/patent/author texts in `Contents/Resources/Licenses/WebRTC`.
