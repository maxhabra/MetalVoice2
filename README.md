# MetalVoice2

A native macOS menu bar microphone utility. **Version 0.9 adds LocalVQE joint noise/room cleanup:** microphone → optional Echo (WebRTC AEC3) → optional Room de-reverb (WPE) → optional WebRTC noise suppression → optional RNNoise, DeepFilterNet3, DPDFNet-2 **or** LocalVQE → clock-corrected transport → an existing virtual device such as BlackHole. Uncheck both checkboxes and set both selectors to Off for pass-through. AEC3 uses native CPU/ARM NEON at 48 kHz. DeepFilterNet3 uses a full-precision streaming Core ML graph with a native Rust/Tract fallback. DPDFNet-2 uses Core ML CPU inference and Accelerate FFTs. RNNoise uses native CPU/ARM kernels. LocalVQE uses native GGML CPU/Accelerate at 16 kHz, with Echo and WPE suspended in that mode.

Requires Apple Silicon, macOS 15+, and an installed virtual audio output. Building requires Swift 6 tools and Rust/Cargo (tested with 1.98.1). The current build uses Command Line Tools; full Xcode is not required. The first build installs pinned Meson/CMake/Ninja into an isolated build directory, builds the vendored AEC library, downloads the locked Rust dependencies and compiles the verified Core ML model using Apple's compiler. Models are included in the repository; the built app works offline without Python or a separate inference service. Python conversion packages are only needed to regenerate the model.

## Build and run

```sh
./scripts/build.sh
open build/MetalVoice2.app --args --show
```

The waveform icon appears in the menu bar. Click it for controls, or right-click / Control-click for Show Controls, Start/Stop, Mute/Unmute, Copy Report, Refresh Devices, and Quit. Actions reflect the current state; Copy Report fetches fresh counters even while the panel is closed. The locally built app is ad-hoc signed; it is not a notarized distribution build.

1. Choose your physical microphone and input channel.
2. Choose **BlackHole 2ch** as the virtual output.
3. Press **Start** and allow microphone access when macOS asks.
4. Select **BlackHole 2ch** as the microphone in the receiving app. Disable that app's suppression during a controlled baseline comparison.
5. Choose a **WebRTC** level: **Off, Low, Medium, High, or Very high**. Medium maps to the same native setting previously labeled Moderate. The dropdown combines enabling and strength selection in a single update. Changes briefly restart the stream and reset per-run latency statistics; saved settings remain compatible.
6. Choose **Enhanced → RNNoise**, **DeepFilterNet3**, **DPDFNet-2**, or **LocalVQE**. Each can run alone (WebRTC Off) or after WebRTC. Start comparisons with WebRTC Off to hear each model’s own processing. DPDFNet-2 uses the full-band 48 kHz HR model; it has a higher processing cost and 50 ms algorithmic delay. Switching to it can take several seconds for Core ML specialization before capture resumes. Existing saved selections are preserved. There is no added strength slider or VAD gate; DeepFilterNet3 uses upstream's default full suppression with its optional postfilter off. Core ML optimization is automatic; **Details → DeepFilterNet3 engine** shows the selected engine and Apple's preferred compute devices. A load failure uses the native reference and is identified as a fallback.
7. Watch **Audio path latency** and the meters. Press **Details**, beside Start/Stop and Mute, for queue, clock correction, dropout counts, and suppression compute time. **Copy report** inside Details copies a JSON snapshot without recording audio.

All suppression engines accept a **48 kHz physical microphone**, matching the tested device configuration. Other input rates can still use pass-through. Select 48,000 Hz in Audio MIDI Setup if needed; the app never changes shared device formats automatically. Output conversion still follows the selected virtual device's rate.

The device selectors share one row: microphone, virtual microphone, then input channel. Full device names are available on hover. Refresh devices and Quit are at the top. The 460-point-wide panel fits its content (normally 316 points high). Echo and Room de-reverb share the first processing row; WebRTC and Enhanced share the second. Details opens a separate section beneath Stop/Mute; the window grows downward while the upper controls stay in place. Closing Details removes that section and its space. Height is clamped to available screen space, with scrolling when needed. Latency measurement boundaries are available on hover and in the copied report.

Capture starts only when you press Start. Closing the popover keeps the transport running; Stop or Quit ends capture. Mute ramps the output to silence while keeping the transport running. Device selections persist. A missing selected device is never silently replaced with another microphone.

Only virtual output devices are offered, to avoid accidental microphone-to-speaker feedback. The app does not install a driver, change default devices, change shared sample-rate/buffer settings, or play microphone audio through speakers. If BlackHole is missing, install it from [the upstream project](https://github.com/ExistentialAudio/BlackHole) before testing this baseline.

The **Echo** checkbox cancels speaker playback picked up by the microphone using **WebRTC AEC3**, independently of the room filter and noise suppressors. It defaults to Off. Enable it and press Start, then allow macOS **System Audio Recording** access. Permission can be managed in System Settings → Privacy & Security → Screen & System Audio Recording (wording varies by macOS version). Microphone permission is still required. All audio stays in memory locally; no recordings are saved.

Echo takes a stereo reference from the **macOS default sound output**. Set the calling app's playback output to that same physical device. The virtual microphone output remains BlackHole. A private, unmuted Core Audio tap captures only the chosen playback device; it excludes this process when Core Audio can resolve its process object. MetalVoice2 sends microphone audio only to its virtual output, not this physical reference route. Virtual/aggregate default outputs are rejected; multi-output/aggregate playback and independently routed apps are not supported in this first implementation. Headphones usually need no Echo cancellation. Details displays the reference device and whether frames are synchronized. Quiet reference frames are valid; a missing/stale/gapped reference stops processing with an actionable error. Device changes rebuild the reference. Echo does not cancel room reverberation from your own voice.

AEC3 adds a measured **430 samples / 8.96 ms** of signal delay, plus one actual tap buffer of queue headroom. It runs before the other processors on the existing worker; capture callbacks only enqueue samples. This is optimized native CPU/NEON DSP, not Metal inference. The stable standalone APM 2.1 package was selected for 48 kHz stereo-reference support and low measured cost; it is not the newest Chromium revision or a claim of universal best quality. See [research and alternatives](docs/RESEARCH.md#speaker-echo-implementation--november-17-2026) and [integration notes](ThirdParty/WebRTCAEC/README.md).

The **Room de-reverb** checkbox (previously labelled Echo) is an experimental, conservative room de-reverberation filter based on online WPE. It estimates late reflections from the microphone itself, with native Accelerate FFTs and bounded spectral attenuation. It is independent of WebRTC and the other Enhanced modes, remembers its setting and briefly restarts processing when changed. It defaults to Off. It needs no playback capture or additional permission, and does **not** cancel audio from Mac speakers. It can reduce room tails, but early reflections and a hollow voice may remain; compare the effect on your voice before leaving it enabled. Details and reports include its 10 ms delay. See [algorithm and reference notes](ThirdParty/NaraWPE/README.md).

## LocalVQE noise and room cleanup

Choose **Enhanced → LocalVQE** for the joint **v1.3 / 4.8M / Float32** model. **Echo** and **Room de-reverb** become unchecked and greyed out while it is selected. Their previous settings are remembered and restored when you switch to another mode. This restriction is enforced in the native engine too. WebRTC stays available; begin with WebRTC Off for a comparison of LocalVQE's own sound.

LocalVQE processes **16 kHz speech**, resampling the 48 kHz microphone before and after inference with filtered, causal conversion. It therefore has less high-frequency bandwidth than the full-band models. It uses a silent far-end input for noise/room cleanup, with the optional noise gate disabled. **Speaker-playback echo cancellation is inactive in this mode**, and no system-audio tap or permission is used. Room-cleanup performance and voice character still need listening comparison on your microphone.

The native GGML CPU backend uses ARM SIMD and Accelerate, with two inference threads selected from M1 Max measurements. This is heavier than RNNoise/DFN3/DPDFNet-2: the complete offline path measured about 4.7 ms average per 10 ms host block and roughly 93% of one CPU core (not 93% of the whole Mac). No Metal/Neural Engine acceleration is claimed for this model. Its **34 ms** signal delay is included in the latency counter; total app latency also includes the transport. The 18.4 MiB model is bundled and verified. Details and Copy report identify the engine, thread count and delay. [Integration details](ThirdParty/LocalVQE/README.md).

## What the latency counter measures

The engine retains the host timestamp of captured samples through bounded queues and a streaming resampler. It adjusts the source origin for the enabled algorithms' alignment delays, all at 48 kHz:

| Processing | Samples | Algorithmic delay |
| --- | ---: | ---: |
| Echo (AEC3) | 430 | 8.96 ms |
| Room de-reverb (WPE) | 480 | 10 ms |
| WebRTC | 334 | 6.96 ms |
| RNNoise | 960 | 20 ms |
| DeepFilterNet3 | 1440 | 30 ms |
| DPDFNet-2 | 2400 | 50 ms |
| LocalVQE (including conversion/framing) | 1632 | 34 ms |
| WebRTC + RNNoise | 1294 | 26.96 ms |
| WebRTC + DeepFilterNet3 | 1774 | 36.96 ms |
| WebRTC + DPDFNet-2 | 2734 | 56.96 ms |
| WebRTC + LocalVQE | 1966 | 40.96 ms |

Echo and Room de-reverb delays add to the selected chain except LocalVQE, where both are inactive. Room de-reverb adds 10 ms; its historical prediction gap adds no further lookahead.

For each valid output block, it compares the first output sample's scheduled Core Audio timestamp with its corresponding source timestamp. The UI shows current, p50, p95, and maximum intervals for the current run. Percentiles use 0.1 ms histogram buckets, sampled once per output callback.

This is **transport timestamp latency**, not a physical mouth-to-listener or receiving-application loopback measurement. It does not independently measure device converter delays, BlackHole's receiving side, conferencing buffers, or network delay. No callback execution-time or hardware buffer-size number is presented as total latency. Missing/invalid timestamps show an unavailable reading; they are not replaced by wall-clock guesses.

Hardware callbacks move samples and perform bounded resampling without application locks, allocation, logging, or UI access. The enabled processors run sequentially on a dedicated serial worker that consumes exact 480-sample frames from a bounded capture queue, without an intermediate queue between stages. Only the selected neural processor has active state, released when the route stops. RNNoise uses its pinned official model compiled into native code. DeepFilterNet3 retains upstream's native FFT/window, ERB construction, feature normalization, mask interpolation and synthesis; one Core ML prediction handles all neural stages with persistent convolution/GRU history. Decoder state advances only when the upstream local-SNR rules enable that stage. Creation, warm-up, resets, inference and destruction stay on one worker. The unmodified Rust/Tract implementation remains available as a fallback/reference. Core ML and libDF can allocate on this worker; no model execution occurs in hardware callbacks. A preallocated 32-tap polyphase sinc resampler supports different output rates and a slow queue controller corrects clock drift. The menu publishes readings at 2 Hz while open; processing and counters continue while closed.

Core ML can select CPU, GPU or Neural Engine according to the model and device. On the tested M1 Max it prefers CPU for the qualified DFN3 predictions; enabling every compute unit does not mean every unit is used. The full-precision optimized DFN3 path roughly halves offline CPU work while matching the original output closely. Half-precision candidates changed local-SNR decisions and were rejected. See [verification results](docs/VERIFICATION.md) for measurements and limits. Faster computation does **not** remove the regular model's 30 ms window/lookahead delay, and this update does not reduce the transport's validated queue margin.

DPDFNet-2 retains the official model’s spectral history, normalization initialization, masking and deep filtering. Its recurrent gate projections are fused and fixed frequency loops unrolled in the Core ML export, with full Float32 arithmetic. Accelerate performs the causal Vorbis-windowed FFT and overlap-add. Details and reports include its 50 ms signal delay. Its engine label is **Core ML · CPU**. This is the configuration qualified on the M1 Max: automatic accelerator placement missed the 10 ms deadline in testing, while the CPU path completed around 1.2 ms per frame with equivalent output. Accelerate uses native optimized DSP routines. Expensive compute-plan inspection is excluded from stream startup. A missing or invalid DPDFNet resource fails Start; it never silently substitutes pass-through.

Suppression mode reserves one additional 10 ms processing block of queue headroom because the worker and output callback can run in either order. This fixed margin prevents the worker/render phase underrun reproduced during live testing; it trades some latency for stability. All enabled stages share this margin. It is separate from their algorithmic delays; all are reflected in the measured audio-path interval. Transport details also show the combined compute time (mean, p99, maximum) for each 10 ms processing block. Compute p99 is an upper bucket boundary at 0.01 ms resolution; if the percentile exceeds 10 ms, it conservatively reports the observed maximum.

## Verify

```sh
./scripts/test.sh
build/MetalVoice2.app/Contents/MacOS/MetalVoice2 --list-devices
```

After granting microphone access in the app, quit the menu app before running a timed benchmark:

```sh
./scripts/benchmark.sh 60 off
./scripts/benchmark.sh 60 moderate
./scripts/benchmark.sh 60 off --enhanced rnnoise
./scripts/benchmark.sh 60 moderate --enhanced rnnoise
./scripts/benchmark.sh 60 off --enhanced deepfilternet3
./scripts/benchmark.sh 60 moderate --enhanced deepfilternet3
./scripts/benchmark.sh 60 off --enhanced deepfilternet3 --df-backend native
./scripts/benchmark.sh 60 off --enhanced dpdfnet2
./scripts/benchmark.sh 60 off --room-dereverb
./scripts/benchmark.sh 60 off --echo
```

The script launches through the app bundle so macOS uses the app's microphone permission. Directly executing the binary from a terminal or automation service may be attributed to a different capture context. Ad-hoc local rebuilds change the code signature, so macOS may require renewed permission for a new build.

The benchmark uses the system's default physical input, channel 1, and BlackHole 2ch. Explicit `--input-uid` and `--output-uid` arguments after the level select another listed route. WebRTC levels are `off`, `low`, `moderate`, `high`, or `very-high`; `--enhanced off|rnnoise|deepfilternet3|dpdfnet2` controls the independent neural stage. Add `--room-dereverb` to enable the room filter or `--echo` for AEC3 after granting System Audio Recording permission in the app. Play through the macOS default physical output for an Echo benchmark. It prints and saves JSON counters under `build/benchmarks`, saves no audio, stops automatically, and refuses to start without existing microphone permission or while the menu app is running. Do not run another writer to the same virtual output, including voice-changer. Reported CPU is the benchmark process's mean use as a percentage of one core; peak resident memory includes its framework/runtime overhead, not a measurement of the open menu UI.

The native tests cover priming, timestamp validity, resampling gain/tone accuracy, overflow rejection, silence and recovery after underruns, simulated independent clocks, 512-to-480 framing, every WebRTC level with the neural backends, upstream processing parity, state reset, algorithm delay, and malformed-input handling. A test-only interposition library verifies allocation-free WebRTC/RNNoise calls; it is never bundled with the app. DeepFilterNet3 has separate Rust reference and Core ML audio comparison tests. DPDFNet-2 has a pinned official-ONNX audio fixture and independent spectral/full-stream reference comparisons, plus native lifecycle and cascade tests. The tests load Core ML through Apple's normal runtime and require access to its compilation/specialization caches; they never open audio devices. These checks do not grant microphone permission or substitute for listening and physical-device tests. Optional speech checks accept normalized mono 48 kHz Float32 raw PCM: `noise-tests --speech PATH` or `coreml-tests build/DeepFilterNet3.mlmodelc PATH`. The default suite uses generated speech at `build/deepfilter-speech.f32` when available, otherwise a deterministic non-stationary synthetic signal.

Pinned dependencies and integration details are documented in [AEC3](ThirdParty/WebRTCAEC/README.md), [WebRTC](ThirdParty/WebRTC/README.md), [RNNoise](ThirdParty/RNNoise/README.md), [DeepFilterNet3](ThirdParty/DeepFilterNet/README.md), [DPDFNet-2](ThirdParty/DPDFNet/README.md), and [NARA-WPE](ThirdParty/NaraWPE/README.md). RNNoise's notes retain the unresolved clarification of its separately distributed model's redistribution terms before public release. DeepFilterNet explicitly licenses its code and model weights under MIT or Apache-2.0. DPDFNet code and model weights are Apache-2.0. License notices are bundled with the app.

Follow [the baseline protocol](docs/BASELINE.md) for measurements and device recovery checks. The broader roadmap is in [PLAN.md](PLAN.md), with algorithm research in [docs/RESEARCH.md](docs/RESEARCH.md).

The scripts select SwiftPM's native build engine and keep build/module caches in the repository's ignored directories. This avoids the full-Xcode and SwiftUI macro-plugin dependencies of the current Command Line Tools' default build engine.

Echo has offline tests for stereo timestamp alignment, clock drift, bounded queues, concurrent producer/consumer wrapping, missing references, impulse delay, silence, near-end preservation, double talk and an echo-path delay change. The v0.8 system-audio permission flow and physical speaker/microphone path still require a live check; synthetic attenuation is not a room-quality guarantee.
