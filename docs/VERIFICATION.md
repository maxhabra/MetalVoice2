# Verification — November 17, 2026

## Version 0.9 — LocalVQE joint noise/room cleanup

Implemented **Enhanced → LocalVQE**, joint **v1.3 / 4.8M / Float32**, with native GGML CPU inference. Echo/AEC3 and Room de-reverb/WPE are suspended in this mode, regardless of native setter order. Both checkboxes become unchecked/disabled; saved preferences return when selecting another mode. WebRTC remains optional. The far-end input is silent and the upstream hard gate is explicitly disabled. This mode does **not** perform reference-based playback echo cancellation and opens no system-audio tap.

Verified on the M1 Max, November 17, 2026:

- Pinned LocalVQE `7f61cd0` and its GGML submodule `c044a8e`; all **1,979** vendored file hashes and the **19,268,160-byte** v1.3 GGUF pass verification. All manifest entries are visible to Git. Only an unused demo's ignore file is amended; DSP sources are unchanged. Apache-2.0/MIT notices and model provenance ship in the bundle. Static CPU/ARM SIMD build has Accelerate enabled; no Metal/ANE inference is claimed.
- The upstream v1.3 regression fixture passes with maximum absolute error **6.077e-7**, mean **8.247e-8**, zero tolerance violations. This compares the native graph numerically with the upstream committed output, not listening quality with Voice Focus.
- New adapter tests cover all eight 10/16 ms scheduling phases and all three resampling phases, in-place/out-of-place equivalence, reset, passband gain/phase through 6 kHz, rejection at 8/9/15/21 kHz, and **ten simulated minutes of FIFO wrap**. Tested out-of-band tone attenuation exceeds 60 dB. This is an adapter soak, not ten minutes of model inference or microphone capture.
- The 193-tap causal resamplers contribute 192 frames, the framing FIFO 672, and model overlap-add 768: **1,632 frames / 34 ms total signal alignment delay** at 48 kHz. The independent processed-waveform correlation peaks at exactly 1,632 samples (correlation 0.9673 on the synthetic signal). WebRTC adds 334 frames, producing **40.96 ms**. Both native stats and pipeline timing exclude AEC/WPE for LocalVQE.
- Model reset reproduces the first run exactly. Silence remains silent, malformed input produces zero output and clears history, all five WebRTC choices match direct composition, WPE is excluded even when requested, and switching to bypass releases model state. Native configuration tests verify incompatibility/restoration in both setter orders. Tests open no audio devices.
- Full regression suite passes: Rust reference, transport, WebRTC/RNNoise, DFN3 Core ML, DPDFNet-2, WPE, AEC3, upstream LocalVQE parity and new integration tests. Two legacy tests were updated because mode 4 is now valid; mode 5 remains invalid. Log: `build/localvqe-full-tests.log`.
- LocalVQE adapter/wrapper/pipeline tests pass AddressSanitizer and UndefinedBehaviorSanitizer. SwiftPM C/C++ application targets were instrumented; externally built GGML, AEC and Rust archives were not. No claim of full dependency instrumentation. Log: `build/localvqe-sanitizers.log`.
- New wrapper/pipeline/test sources pass `-Wall -Wextra -Werror` syntax checks. Bundle resource lookup was tested in a small generated app using the packaged model and production wrapper, including warm-up and processing, with no capture. Log: `build/localvqe-bundle-test.log`.
- Production SwiftUI rendered with mock devices: collapsed **460 × 316**, expanded **460 × 622**, collapsed again **460 × 316**. Both checkboxes are visibly grey and unchecked. Controller tests preserve/restore both preferences and block their setters in LocalVQE mode. The actual NSPopover keeps its top and horizontal coordinates across five expansion states. Logs: `build/localvqe-ui.log`, `build/localvqe-popover.log`; images: `build/ui-preview/localvqe-*.png`.

Offline processing-only timing (M1 Max; these are not whole-app utilization or power readings):

| Native model threads | Mean / 16 ms model hop | p99 | CPU work per audio second, one core = 100% |
| --- | ---: | ---: | ---: |
| 1 | 12.992 ms | 14.069 ms | 81.5% |
| **2, selected** | **7.284 ms** | **8.270 ms** | **90.1%** |
| 4 | 4.408 ms | 7.563 ms | 106.6% |

Complete two-thread conversion/model path: **4.745 ms average, 8.062 ms p99, 8.195 ms maximum** per 10 ms host block; **93.4% of one core** in the dedicated offline run. Some blocks require no inference. The full-suite run measured 4.830 ms average / 8.348 ms p99 with a **19.445 ms maximum outlier**, so a strict 10 ms execution deadline is not guaranteed under contention. Existing bounded transport headroom is retained. Test-process peak RSS was 61–63 MiB with multiple contexts used in the tests; this is not the whole menu app's memory footprint. Logs: `build/localvqe-tests.log`, `build/localvqe-full-tests.log`.

Version **0.9.0 (10)** is built and locally ad-hoc signed. **Live listening/route qualification remains pending**: no microphone/playback capture was started, no audio was recorded, and the existing running app was not interrupted. Reopen the rebuilt app to load the new version. Compare LocalVQE first with WebRTC Off, especially quiet speech, peaks, room tails, and the reduced treble from 16 kHz model bandwidth. See [live comparison protocol](BASELINE.md#localvqe-live-comparison-v09-pending). Numerical parity does not establish that LocalVQE sounds better than sup2 or the existing models.

## Version 0.8 — independent speaker Echo and Room de-reverb

Implemented native WebRTC AEC3 through pinned webrtc-audio-processing 2.1, with a macOS Core Audio stereo playback reference. The former Echo checkbox is now **Room de-reverb**, preserving its existing WPE implementation and saved setting. Echo is a new, independent checkbox, Off by default. Both appear above the WebRTC/Enhanced row.

Verified on the M1 Max, November 17, 2026:

- Source hashes: 2,205 vendored WebRTC/Abseil files; all manifest entries are visible to Git, including the Meson-patched Abseil source. Static Meson build uses the vendored dependency, ARM NEON, and macOS 15 deployment target. Original archive checksums and notices are retained.
- Full existing regression suite passed after isolating the standalone WebRTC NS namespace from the full APM: Rust reference, transport, WebRTC/RNNoise, DFN3 Core ML, DPDFNet-2, WPE reference parity and all 20 WPE/noise chains. No source/model changes to those processors. Log: `build/echo-full-tests.log`.
- New offline Echo tests pass: anti-phase stereo reference preservation, exact block boundaries, stale/future/gapped data, malformed input, overflow/discard, 44.1/48/96 kHz reference interpolation, ±200 ppm drift, and a concurrent 480,000-sample producer/consumer wrap test. Production tap format is explicitly 48 kHz; the additional queue-rate tests exercise alignment independently.
- AEC3 impulse peak delay is **430 samples / 8.958 ms**, consistent across five impulse positions. Reset succeeds; non-finite microphone input fails closed with zeroed output.
- Deterministic 24-second synthetic scenarios include independent stereo signals, echo delays changing from 30 to 50 ms, reflected taps, local voice alone, and simultaneous local voice/playback. After adaptation, far-end echo attenuation was **20.02 dB mono** and **17.57 dB stereo**. During double talk, delayed local-signal correlation was **0.921 / 0.936** and projected gain **0.805 / 0.822**. With silent playback, correlation was **0.991**, projected gain **0.937**. This demonstrates useful attenuation and measurable voice changes, not perfect preservation or a listening-quality score.
- Release AEC processing on this machine: p50 **0.17–0.25 ms**, p99 **0.24–0.38 ms** per 10 ms frame in those offline cases. These exclude Core Audio capture, UI, other filters and startup; they are not whole-app CPU or power measurements. Native SIMD CPU execution is implemented; no Metal or neural-residual-echo inference is claimed.
- AEC/reference-queue tests also passed AddressSanitizer and UndefinedBehaviorSanitizer. App wrappers and queue code were instrumented; externally compiled WebRTC/Abseil and Rust archives were not. This is not full dependency instrumentation or an OS callback-lifecycle test. Logs: `build/echo-sanitizers-final.log`, `build/aec-tests.log`.
- Our C++/Objective-C++ sources pass `-Wall -Wextra -Werror` syntax checks, treating third-party headers as system headers. Bundle plist and shell scripts validate. Version **0.8.0 (9)** builds and passes local code-sign verification; it includes `NSAudioCaptureUsageDescription` and AEC/Abseil/DSP license notices.
- Rendered the production SwiftUI layout with mocked devices: collapsed **460 × 316** points; Details grows and collapses without residual space. The actual native NSPopover was exercised through five repeated states; its top and horizontal coordinates stayed identical. The viewport remains scrollable when expanded details exceed available space. Logs: `build/echo-ui.log`, `build/echo-popover.log`; images under `build/ui-preview/echo-*.png`.

**Not yet qualified live:** this update did not open a microphone or system-audio tap, interrupt the user's existing running app, or save audio. The new System Audio Recording permission flow, tap sample timestamps under physical playback, actual speaker-to-microphone cancellation, default-output switching, permission denial/revocation and teardown under hardware failure still need the [live protocol](BASELINE.md#speaker-echo-live-qualification-v08-pending). The implementation has explicit error handling for these conditions; offline tests do not prove OS behavior. Calling-app playback must use the macOS default physical output; virtual/aggregate playback routes are rejected. The user must reopen the rebuilt app to load it. An ad-hoc signature change can require renewed microphone permission too.

The latency counter includes AEC's signal delay and actual reference-buffer queue headroom (128 frames requested; actual value queried), in addition to existing transport and selected-filter delays. It remains a Core Audio timestamp measurement, not physical mouth-to-listener latency. A valid silent reference is allowed; a missing/gapped/stalled reference stops with an actionable error rather than silently treating uncancelled audio as success.


## Completed

- Built the arm64 release application using Swift 6.4 Command Line Tools and SwiftPM's native build engine. Version 0.3 statically links the pinned WebRTC NS subset and RNNoise with its official regular model, using upstream ARM NEON code.
- Created `build/MetalVoice2.app`, ad-hoc signed it, and verified its signature.
- Validated the bundle plist and shell-script syntax.
- Passed native transport tests for priming, timestamp preservation/absence, DC gain, reference-tone resampling, underflow silence/recovery, overflow rejection, and bounded resynchronization.
- Passed a concurrent producer/consumer check over 1.28 million rendered samples. The transport test suite also passed AddressSanitizer and UndefinedBehaviorSanitizer; this does not replace a thread-sanitizer or physical-device test.
- Native audio sources passed a `-Wall -Wextra -Werror` syntax check.
- Passed 44.1 → 48 and 48 → 44.1 kHz simulations. Passed 120-second **simulated** independent-clock runs at +500/−500 ppm (48 kHz) and +300 ppm (44.1 → 48 kHz). These are not hardware-soak results.
- Read actual device metadata outside the execution sandbox: the default Fulla Schiit microphone and BlackHole 2ch are available, both reporting 48 kHz and 512-frame buffers. No device settings were changed.
- Verified all 39 upstream WebRTC source files against their pinned Git blob hashes. Retained WebRTC and Ooura license notices in the app bundle.
- Tested all four suppression levels, silence, finite normalized output, synthetic stationary-noise attenuation, 512-to-480 sample framing/timestamps, and zero C++ heap allocations in processing calls. The noise tests passed AddressSanitizer and UndefinedBehaviorSanitizer.
- Measured the assembled suppressor's 334-sample impulse delay at all four levels and across several filter/frame phases.
- Reproduced a worker/render scheduling underrun in a deterministic test. Reserving one additional processing hop in suppression mode passes that regression test.

## RNNoise integration verification

- Verified 36 pinned RNNoise source/model files. The model archive SHA-256 matches upstream `model_version`; source/model provenance and model redistribution clarification are recorded in `ThirdParty/RNNoise/README.md`.
- Compared all ten configurations (WebRTC Off/Low/Moderate/High/Very high × RNNoise Off/On) with direct upstream calls on identical non-silent input. Outputs match sample-for-sample, including scale conversion, ordering, and persistent state.
- Verified reset removes prior audio history, invalid configuration is rejected, and malformed/extreme input does not poison RNNoise state or produce non-finite/out-of-range output.
- Measured RNNoise's 960-sample impulse alignment at several frame offsets. Broadband correlation through WebRTC → RNNoise peaks at 1294 samples at every WebRTC strength. A single isolated impulse can acquire a later dominant tail through this nonlinear cascade, so it is not used as the cascade-delay oracle.
- Verified no C or C++ heap allocations in steady processing with a test-only allocator interposition library. The test explicitly proves its C allocator hook is active. This library is not bundled with the app.
- Passed the complete native noise suite under AddressSanitizer and UndefinedBehaviorSanitizer, including RNNoise. C allocator interposition is disabled for that separate sanitizer run.
- The final native audio sources and test allocation helper pass `-Wall -Wextra -Werror` syntax checks.
- Rendered the actual SwiftUI menu in an offscreen AppKit preview with mock devices, WebRTC Very high, and RNNoise selected. Both controls, latency, meters, and Start/Mute fit the 380 × 600 viewport; remaining details scroll. This verifies layout, not a live microphone or device-selection interaction. Preview: ignored `build/ui-preview/menu.png`.

Release **offline processing-only** measurements on this Mac, after 300 warm-up frames and over 900 measured 480-sample frames of deterministic non-silent Gaussian noise:

| Processor | Compute p50 | Compute p99 | Compute maximum |
| --- | --- | --- | --- |
| RNNoise | 0.635 ms | 0.740 ms | 0.782 ms |
| WebRTC Moderate → RNNoise | 0.643 ms | 0.765 ms | 0.792 ms |

These are timed processing calls, not end-to-end latency, a live CPU/energy benchmark, or speech-quality evidence. Full test output is in ignored `build/rnnoise-test.log`; sanitizer output is in `build/rnnoise-sanitizers/result.log`. The native app bundle is approximately 15.5 MB; this is disk size, not resident memory.

RNNoise adds 20 ms of algorithmic delay; the combined chain adds 26.9583 ms. Both use the same worker/queue headroom. The UI and JSON report include each stage's delay and total mean/p99/maximum compute time. RNNoise defaults to Off and the existing saved WebRTC setting is preserved.

## Live measurements

Actual route: Fulla Schiit microphone, channel 1 → BlackHole 2ch; 48 kHz, 512-frame hardware buffers. Runs used the headless benchmark through LaunchServices, with the menu app quit. No audio was recorded. CPU percentages refer to one core and include the benchmark process; memory is peak process residency, not an open-menu measurement.

| Build / mode | Duration | p50 / p95 | Mean CPU | Peak residency | Underruns |
| --- | --- | --- | --- | --- | --- |
| Pass-through | 20.09 s | 38.85 / 39.05 ms | 0.95% | 20.89 MB | 0 |
| Initial WebRTC Moderate | 30.12 s | 50.65 / 50.95 ms | 1.97% | 20.94 MB | 1 |
| Initial WebRTC Moderate repeat | 45.16 s | 50.25 / 52.75 ms | 1.66% | 20.96 MB | 1 |

The two initial suppression runs are diagnostic failures, not a stability claim. They motivated the worker queue-headroom fix. The user subsequently tested the rebuilt WebRTC app and reported that it works well and the settings sound very good. A final automated live run was not performed; these historical measurements are not results for the fixed worker or RNNoise. Historical raw counters remain under the ignored `build/benchmarks` directory.

The extra queue headroom is 480 samples / 10 ms in suppression mode. It covers a worker-produced block arriving on the other side of an output callback. It adds delay in exchange for scheduling margin and is included in the timestamp readout, separately from the 6.96 ms algorithmic delay.

## Practical limits and remaining checks

- The user confirmed that the native baseline, corrected popover placement, WebRTC, and RNNoise sound good. Automated RNNoise/DeepFilterNet3 physical-device, foreground-load, and live CPU/memory measurements remain to be performed; these integrations did not start microphone capture.
- Stationary-noise tests establish functional suppression, not subjective quality on the user's speech or equivalence to browser processing/Voice Focus.
- USB hotplug, sleep/wake, sample-rate changes, foreground-load tests, energy measurements, eight-hour stability, and receiving-app/physical loopback still require qualification.
- Direct command-line execution from the automation runner could not use the app's microphone grant. LaunchServices execution worked after permission was granted and the menu app quit. Local ad-hoc rebuilds can require permission again.

After granting the current build microphone access, quit the menu app and use:

```sh
./scripts/benchmark.sh 60 off
./scripts/benchmark.sh 60 moderate
./scripts/benchmark.sh 60 off --enhanced rnnoise
./scripts/benchmark.sh 60 moderate --enhanced rnnoise
./scripts/benchmark.sh 60 off --enhanced deepfilternet3
./scripts/benchmark.sh 60 moderate --enhanced deepfilternet3
```

Do not infer end-to-end latency from the 512-frame hardware buffer or the configured software queue. See [BASELINE.md](BASELINE.md) for measurement boundaries and the test protocol.

## Compact controls — version 0.3.1

- Replaced the stacked route controls with one row: microphone, virtual microphone, input channel. Full device names remain available in the menus and tooltips.
- Replaced the WebRTC checkbox/strength pair with one Off/Low/Medium/High/Very high selector. Medium retains the old Moderate mapping. Both saved enable/strength values are updated before one possible stream restart.
- Placed WebRTC and Enhanced on one row; moved latency measurement explanation to hover help; made the latency value and label share a row with compact inline meters below.
- Rendered the actual SwiftUI layout with mock device/running/latency data at 460 × 400. All main controls and footer actions fit without scrolling; expanded diagnostics still scroll. Preview: ignored `build/ui-preview/compact-menu.png`.
- Added a native menu bar right-click/Control-click menu with state-dependent Start/Stop, Mute/Unmute, Show Controls, Copy Report, Refresh Devices, and Quit. Copy Report requests a fresh engine snapshot independently of the panel's visibility.
- Audio processing and transport code were not changed in this UI update. The user reports that the RNNoise build works very well; no microphone capture or new live benchmark was started during this update.

## DeepFilterNet3 and stable controls — version 0.4.0

- Added Enhanced → DeepFilterNet3 using the pinned regular upstream model and unmodified Rust libDF sources. Verified all 23 source/model files; statically linked Tract 0.21.4 with native ARM kernels. No Python, external service, model download at launch, or Core ML/Metal inference is involved. Only the selected neural backend has active state.
- The native worker owns the complete processor lifetime: creation, warm-up, processing, resets, and destruction. Startup waits for readiness before starting capture. This respects Tract's thread-affine, non-`Send` state. Hardware callbacks never perform model inference.
- Rust tests compare the C bridge sample-for-sample with a separate upstream runtime over repeated resets, check malformed buffers, and verify the 1440-sample / 30 ms alignment delay across four impulse offsets. The alignment test uses a test-only low attenuation limit to expose original-signal timing; production uses full upstream suppression with the optional postfilter off.
- Native integration tests cover DeepFilterNet3 alone and after every WebRTC strength, state reset, switching from RNNoise, silence, malformed input, normalized bounded output, and backend/delay reporting. Combined WebRTC + DeepFilterNet3 delay is 1774 samples / 36.9583 ms. Worker headroom and device/transport latency remain additional components.
- Transport and existing WebRTC/RNNoise tests still pass. C/C++ audio code and noise tests pass AddressSanitizer + UndefinedBehaviorSanitizer; the linked Rust/Tract release library is **not sanitizer-instrumented**. WebRTC/RNNoise allocation checks remain separate: Tract/libDF allocates temporary tensors on the worker.
- Tested 9.265 seconds of macOS-generated speech with added Gaussian noise and 60 Hz hum. No microphone was opened or recorded. Alignment-corrected clean/output correlation was 0.892 for DFN3 and 0.920 for WebRTC Medium + DFN3. This is a speech-preservation regression check, not a perceptual score or proof that stacking sounds better.

| Offline noisy-speech processing | Compute p50 | Compute p99 | Compute max |
| --- | ---: | ---: | ---: |
| DeepFilterNet3 | 0.718 ms | 1.058 ms | 1.206 ms |
| WebRTC Medium + DeepFilterNet3 | 0.192 ms | 1.011 ms | 1.204 ms |

These are timed 480-sample processing calls on the M1 Max, excluding model initialization, not live CPU use or total audio latency. Upstream's conditional inference makes timing input-dependent; the low median after WebRTC is not a universal speedup. A separate DFN3-only native probe initialized in about 262 ms and used 57.0 MB resident memory including its runtime; that is not the complete menu app's memory. The bundled app is approximately 40.9 MB on disk, including model/runtime and license notices.

- Moved Refresh devices and Quit to the header, removed the routing reminder, and placed Details beside Start/Stop and Mute. Copy report is inside Details and remains available in the right-click menu.
- Rendered the actual SwiftUI view with mock running/device data and DeepFilterNet3 selected. Opening and closing Details retained the 460 × 400 host/window geometry. Pixel comparisons verified unchanged header, selectors, latency, meters, Stop/Mute, and status positions; collapsing restored the original image. Only diagnostics scroll. This is an offscreen layout check, not a live device test.
- App packaging signs in a temporary staging directory to avoid synced-folder Finder metadata interfering with signing, then replaces the executable atomically. The staging bundle passes strict signature verification; the installed repository bundle passes signature verification. The build/test scripts relink generated executables when the external Rust library changes.
- Logs: ignored `build/deepfilter-final-tests.log`, `build/deepfilter-speech-test.log`, and `build/deepfilter-sanitizers/result.log`. UI previews are under `build/ui-preview/deepfilter-*.png`. Dependency provenance, runtime contracts, and notices are in [the DeepFilterNet integration notes](../ThirdParty/DeepFilterNet/README.md).

DeepFilterNet3 still needs the user's listening test, real-device start/stop and switching checks, and live CPU/energy/latency measurements under foreground load. No live performance or long-run stability result is claimed for this version.

## Core ML optimization — version 0.5.0

The user confirmed that version 0.4's DeepFilterNet3 works. This update preserves the regular model, its full-precision arithmetic, and its 30 ms alignment delay while optimizing inference and temporary work.

- Exported the pinned ONNX weights into one Core ML prediction per 480 samples. Temporal convolution caches and five GRU states persist explicitly in Float32. Native input arrays/feature providers are reused. Core ML conditionals skip inactive decoder computation and retain the same history as upstream.
- Preserved upstream FFT/window, normalization, ERB bands, mask interpolation and synthesis through actual libDF calls. The new five-frame spectrum adapter matches complete upstream `DfTract` output sample-for-sample in the Rust oracle test, including silence and recovery.
- Checked a full-precision streaming graph against upstream ONNX Runtime sequences: maximum error about 2.7e-6 in gains, 1.1e-6 in coefficients, and 1.5e-5 in local SNR. The shipped graph additionally implements upstream's conditional decoder execution, validated by whole-audio comparisons.
- Rejected pure and mixed Float16 candidates: small local-SNR changes crossed hard stage thresholds and caused materially larger output differences. The shipped model uses Float32 history and arithmetic. The macOS 15 Core ML export only supports Float16 `MLState`, so the verified version uses explicit Float32 state instead.
- Passed complete Core ML/native audio comparisons with every WebRTC strength, speech/noise/silence/recovery, repeatable reset, switching from RNNoise, malformed input, delay reporting and explicit native fallback after model-load failure. Largest normalized absolute output difference was 1.68e-6; relative RMS difference at most 1.28e-6 on this fixture. Reset reproduced the same output exactly.
- Passed the existing transport, WebRTC/RNNoise allocation and native DFN3 tests, plus all four Rust tests. Passed C/C++/Objective-C++ AddressSanitizer and UndefinedBehaviorSanitizer through both native and Core ML audio tests. Rust and Apple's system frameworks are release libraries, not sanitizer-instrumented.
- Core ML permits all compute units and uses fast-prediction specialization. On the tested M1 Max, its compute plan selects CPU. Full/half precision and several state layouts were examined with CPU, CPU/GPU, CPU/Neural Engine and automatic configurations; enabling those options did not establish actual GPU/ANE execution. No custom Metal or Neural Engine speedup is claimed.

Final offline release measurements used generated speech with Gaussian noise and hum, plus silence and recovery, after compiler/sanitizer runs finished:

| Mode | Native p50 / p99 | Core ML p50 / p99 | Native / Core ML process CPU |
| --- | ---: | ---: | ---: |
| DFN3 alone | 0.704 / 1.088 ms | 0.250 / 0.508 ms | 6.71% / 2.99% |
| WebRTC Medium + DFN3 | 0.188 / 0.972 ms | 0.201 / 0.457 ms | 4.44% / 2.80% |
| WebRTC Very high + DFN3 | 0.190 / 0.947 ms | 0.209 / 0.459 ms | 4.39% / 2.78% |

CPU is this offline process's CPU seconds divided by represented audio seconds, expressed as a percentage of one core. It is not a live menu-app measurement, energy measurement or accelerator utilization. Native medians after strong WebRTC can be lower because many frames skip the expensive decoders; total CPU work and p99 still improve in the optimized path. A separate single-backend probe measured 59.7 MB peak residency for native versus 47.9 MB for Core ML, with initialization about 272 versus 349 ms. Those memory figures include the probe/runtime, not the full menu UI. The app bundle is approximately 49.5 MB on disk, including the fallback runtime and all license notices.

Hardware capture buffers, output scheduling and worker headroom remain at the previously validated settings. Faster neural computation does not remove the 30 ms window/lookahead delay, and there is no new physical or receiving-app latency measurement. No microphone capture was started during this update; live listening, energy/load tests and long-run qualification remain separate.

The release app includes the compiled, hash-verified model and is signed/verified. The Details panel and copied JSON report show Core ML's preferred devices or identify native fallback. The offscreen SwiftUI preview retains identical control/window geometry across Details toggles. Logs are in ignored `build/coreml-performance.log`, `build/coreml-final-tests.log`, `build/coreml-resource-*.log` and `build/coreml-sanitizers/*results.log`.

## DPDFNet-2 — version 0.6.0

Added the official **DPDFNet-2 48 kHz HR** model as Enhanced → DPDFNet-2,
alone or after WebRTC. Existing selections remain unchanged. The model and
empirical state initialization are bundled; no Python, inference server or
runtime downloads are needed. Source/model revisions, Apache-2.0 license and
hashes are recorded in `ThirdParty/DPDFNet`.

The full Float32 streaming Core ML conversion fuses recurrent gate projections
and unrolls fixed frequency sequences. Native Accelerate handles the 960-sample
Vorbis-windowed FFT and overlap-add. Only the selected backend runs, on the
existing worker. The 480-sample processing hop does **not** imply 10 ms signal
delay: independent full-stream correlation and the model's spectral buffers
establish **2,400 samples / 50 ms**, or 56.9583 ms with WebRTC. The UI, source
timestamps and JSON counters include this delay.

The shipping configuration explicitly uses Core ML **CPU**. Automatic placement
missed the 10 ms deadline during unrestricted qualification; the CPU path has
substantial processing headroom. The app reports the CPU constraint, without a
GPU/ANE utilization claim. Compute-plan inspection of the large unrolled graph
was removed from startup because its asynchronous analysis could compete with
audio after the timeout. First model specialization can take several seconds.

Final native release results on the M1 Max, using the five-second pinned
generated-speech/noise fixture, with no concurrent sanitizer run:

| Mode | Compute p50 | Compute p99 | Offline CPU, one core |
| --- | ---: | ---: | ---: |
| DPDFNet-2 | 1.076 ms | 1.319 ms | 11.13% |
| DPDFNet-2 after reset | 1.014 ms | 1.180 ms | 10.40% |
| RNNoise | 0.628 ms | 0.682 ms | 5.29% |
| DeepFilterNet3 Core ML | 0.197 ms | 0.426 ms | 2.58% |

These are offline compute measurements, not live app energy or total audio
latency. The test process reached 128.1 MiB peak residency across loading,
switching and simultaneous reference instances; this is not the menu app's
steady-state memory. The built bundle is approximately 58.6 MiB on disk.

Validation completed:

- Independent official-ONNX spectral comparison: maximum absolute difference
  3.72e-8. Full generated-speech/noise/silence/recovery stream: maximum audio
  difference 5.96e-7, relative RMS error 1.76e-7, measured lag exactly 2,400 samples.
- Pinned native audio fixture: maximum difference 2.38e-7; relative RMS error
  1.68e-7. Reset repeats samples exactly. Every WebRTC strength matches a separate
  cascade. Switching through RNNoise/DFN3 and returning to DPDFNet resets state.
- Missing resources fail initialization, raw malformed buffers are rejected,
  capture inputs are sanitized, and backend/delay counters are correct.
- AddressSanitizer + UndefinedBehaviorSanitizer passed for the C/C++/Objective-C++
  path. Apple frameworks and the existing Rust release library are not instrumented;
  leak detection is disabled for the framework-loaded test process.
- Existing transport, WebRTC/RNNoise allocation and native DFN3/Rust tests passed.
  The DFN3 Core ML regression suite passed with normal macOS service access. In
  the restricted automation sandbox, its conditional graph failed with IOSurface
  errors; the same failure reproduced in the unchanged old test binary. This
  was not accepted as a passing test until the unrestricted run matched upstream.
- The offscreen menu fits the new option. Window and host dimensions stay fixed
  across Details toggles; the header, device selectors, filters, latency, meters,
  Stop/Mute and status regions are pixel-identical, and collapse restores the
  original image. The preview uses fixed mocked devices and no hardware capture.
- Build provenance and bundled model/state/notices match. The app is ad-hoc
  signed and passes `codesign --verify`; the temporary staging bundle also passes
  strict verification. The synced Documents provider re-adds Finder metadata to
  model directories in the destination, so later `--strict` checks there may
  reject that metadata despite unchanged signed contents.

Rejected experiments include generic GRU loops (higher processing cost), Apple's
native GRU conversion (slower and less accurate in this test), and Float16
(about 6.2% relative audio error on the fixture, also slower). The shipping model
retains full precision and the official weights.

No live microphone capture was started. Physical route, receiving-app latency,
long live sessions and listening quality remain separate checks. Logs are in
ignored `build/dpdfnet-cpu-qualified.log`, `build/dpdfnet-qualified-tests.log`,
`build/dpdfnet-sanitizers.log`, `build/dpdfnet-dfn-unsandboxed.log` and
`build/dpdfnet-ui-compare.log`.

The backend follows Apple's [compute-unit selection](https://apple.github.io/coremltools/docs-guides/source/load-and-convert-model.html), [state model contract](https://apple.github.io/coremltools/docs-guides/source/stateful-models.html), and [compute-plan inspection](https://developer.apple.com/documentation/coreml/mlcomputeplan) APIs. Preferred-device reporting describes the plan rather than measuring hardware activity.

## Room de-reverberation and compact panel — version 0.7.0

The user clarified that “Echo” means room reverberation / hollow voice. The new
checkbox therefore runs conservative single-channel online WPE before WebRTC
and the selected enhanced processor. It is independent and defaults to Off.
It is not speaker-playback AEC; no system audio is captured or new capture
permission requested. The MIT NARA-WPE reference is pinned and its notice is
bundled. The app remains native and requires no additional model or service.

Audio verification on the M1 Max:

- Complete `scripts/test.sh` passed, including the existing transport, Rust,
  WebRTC/RNNoise, Core ML DFN3 and official-ONNX DPDFNet tests.
- Native room output versus a pinned NARA-WPE update and independent NumPy
  analysis/synthesis/mask: max absolute error 2.38419e-7; relative RMS 1.25515e-7
  across a 5.51-second synthetic reverberant fixture. Fixture/source hashes
  are verified by the normal build/test scripts.
- A startup impulse confirms unity gain and **480 samples / 10 ms** alignment.
  Timestamp origins, Details and JSON reports include this delay, including
  room-only mode. All 20 WebRTC-level/enhanced-mode combinations match the same
  processors run in order. Switching/restarting clears room history.
- Exact reset repeatability, malformed-input rejection, a 120-second simulated
  changing-level/silence stream, and zero streaming heap allocations passed.
  The 120 seconds refers to represented audio, not a hardware soak.
- Address and undefined-behavior sanitizer runs passed. System frameworks and
  the existing release Rust library are not sanitizer-instrumented; leak
  detection was disabled. Allocation tracking is checked separately in the
  normal release run.
- Release room-test timing with allocation instrumentation: median 0.436 ms,
  p99 0.679 ms, maximum 0.783 ms per 10 ms block. These are offline compute
  measurements, not total audio latency or live application CPU measurements.

The separate `scripts/check_room_quality.py` probe used the existing generated
Samantha speech fixture (9.27 seconds) with deterministic room responses. After
480-sample alignment and excluding the first second, input → output SI-SDR was
4.37 → 5.59 dB for a 0.3-second decay, 2.30 → 3.48 dB for 0.6 seconds, and
1.10 → 2.13 dB for 0.9 seconds. This is a modest objective improvement on three
simulations, not perceptual qualification. The dry control was altered too
(output SI-SDR 22.41 dB; level change −0.45 dB), so the checkbox is experimental
and should be compared by ear. No live microphone content was recorded or used
for these checks. Real-room listening and live device/energy qualification
remain to be done by the user.

UI verification:

- Actual SwiftUI renders at 460 × 290 points collapsed, with Echo, WebRTC and
  the longest enhanced option on one row. Details adds a lower section (596
  points total in the mocked running configuration) instead of reserving space.
- Pixel comparisons confirm unchanged header, devices, filters, latency,
  meters, Stop/Mute and status positions; collapsing restores identical pixels.
- Tested the production resize method with an actual NSPopover and temporary
  visible anchor. Repeated content heights 290 → 577 → 290 → 577 → 290 kept the
  same window top and horizontal origin. Different detail settings account for
  the 577/596 height difference. Automatic dismissal was disabled in this test
  harness; production keeps its normal transient behavior.
- Resizing is deferred until SwiftUI's measurement pass completes, then only
  the hosting controller's preferred size and popover content size are updated.
  Manually forcing the window frame during measurement could override AppKit's
  new size; that approach was removed. Available screen height still caps the
  panel and enables scrolling when necessary.

Logs and previews are under ignored `build/room-*.log` and `build/ui-preview/`.
