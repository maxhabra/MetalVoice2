# Pre-1.0 audio-path review

Reviewed the v0.9 source tree at `b3ccfc2` on an M1 Max. There were no later application commits in the checkout. These five files control what happens when audio goes missing, a device changes, a sample rate differs, or inference approaches its deadline.

## Fixes and verification

| Original finding | Change in the current build | Verification |
|---|---|---|
| A capture gap could reach the denoiser | Timestamp each callback span. Discard an incomplete hop at a discontinuity and reset the processor before using the new segment. | The original 256 + 224 sample reproduction, ring wrap, exact-hop gaps, timestamp loss/recovery and queue discard pass. |
| An unrelated device change interrupted a call | Recheck the active device IDs, UIDs, channel counts, rates and buffer sizes on the control queue. Track inventory changes separately. Set the silence flag from the whole block's peak so the mute fade reaches the output. | A simulated HAL keeps the stream running through an unrelated hot-plug, detects buffer changes and mic removal, and delivers the 128-sample mute transition. |
| A transient open failure cancelled reconnection | Return a typed result for an unavailable HAL device. Retry at 1, 2, 4, then 8 second intervals until recovery or Stop. | Native open/reopen and Swift recovery tests cover a transient error, success, explicit cancellation and permanent failure. |
| Strong downsampling aliased into speech | Scale the sinc filter length with the downsampling ratio and adjust buffer headroom and phase count. Equal rates retain the 32-tap filter. | At 48 → 8 and 192 → 8 kHz, a 5 kHz tone was 103.9 and 103.8 dB down. Passband error at 3.2 kHz was under 0.002 dB. Resampler-only p99 for 10 ms of output was 0.010 ms at 48 → 48, 0.432 ms at 48 → 8 and 0.178 ms at 192 → 8 in one M1 Max run. |
| LocalVQE created a worker every hop and ignored backend failures | Use a model-owned, sleeping CPU worker pool. Propagate a failed compute, return silence and clear partially updated state. | An instrumented release build created zero threads across 100 warm hops. Injected streaming and batch failures returned an error and silence, and the next streaming output matched a fresh context. |

The complete release suite passed on the fixed build. Rerendering the same 40 clips reproduced every per-clip PESQ and STOI value across all nine modes exactly. LocalVQE measured 31.08 ms compute p99 per 10 ms host block in that speech run. Pool reuse removed worker creation during processing, but this model can still exceed its deadline.

Run `./scripts/test.sh` to reproduce the regressions. Device events in these tests use a simulated HAL. The physical-device checks are listed under Release qualification.

## Original review of v0.9

The following findings refer to the pinned v0.9 sources.

## 1. CaptureQueue.hpp — a gap can pass through the denoiser

**High · reproduced offline.** The producer stores a timestamp for every sample. The consumer copies 480 samples and reads only the first timestamp. The worker checks continuity between the beginnings of successive blocks. A gap *inside* a block therefore reaches the model before the worker can detect it. AEC can also pair the latter part with the wrong playback samples. [Queue implementation](https://github.com/maxhabra/MetalVoice2/blob/b3ccfc2fc1d21beb065732d2d9c139edb4df8fe8/Sources/AudioCore/include/CaptureQueue.hpp#L22) · [Worker](https://github.com/maxhabra/MetalVoice2/blob/b3ccfc2fc1d21beb065732d2d9c139edb4df8fe8/Sources/AudioCore/AudioCore.cpp#L189)

**Reproduction.** At 48 kHz, enqueue 256 samples, advance the next callback timestamp by an extra 480 frames (10 ms), then enqueue 224 samples. A single pop returned both segments with the original timestamp and zero overruns. A device timestamp jump requires no queue overflow.

**Change and test.** Track continuity at capture-buffer boundaries. Discard an incomplete pre-gap hop, reset the model before consuming the first complete post-gap hop, and propagate the discontinuity to transport. One timestamp and sample count per contiguous span could also reduce callback memory traffic. Preserve single-producer/single-consumer ownership. Test a gap inside a hop, exactly on a boundary, across ring wrap, and when valid timestamps resume.

## 2. AudioCore.cpp — an unrelated device can interrupt a call

**High · traced through the native listener and controller.** Any change to the system device list sets the same flag as losing the selected microphone. The controller responds by stopping the route and rebuilding the processors. Connecting an unused headset can therefore interrupt a call and discard model history. Model initialization makes this particularly costly with Core ML. [Listener](https://github.com/maxhabra/MetalVoice2/blob/b3ccfc2fc1d21beb065732d2d9c139edb4df8fe8/Sources/AudioCore/AudioCore.cpp#L233) · [Registrations](https://github.com/maxhabra/MetalVoice2/blob/b3ccfc2fc1d21beb065732d2d9c139edb4df8fe8/Sources/AudioCore/AudioCore.cpp#L482) · [Restart decision](https://github.com/maxhabra/MetalVoice2/blob/b3ccfc2fc1d21beb065732d2d9c139edb4df8fe8/Sources/MetalVoice2/AudioController.swift#L431)

**Change and test.** Refresh the device list after a global notification. On the control queue, compare the selected input, channel availability, output, rates and buffer sizes, plus the physical playback device when Echo is active. Rebuild only when that active route changes. A test should connect an unrelated device during a call and verify uninterrupted frame counts, then remove the selected device and verify recovery.

**Also in this file.** On a 512-frame mute transition the first 127 samples can contain the fade, yet the callback marks the whole buffer silent because the final gain reached zero. A downstream consumer can skip the fade. Use the already-computed output peak to identify a fully silent block. Test the flags and samples for both the transition and the following block. [Mute ramp](https://github.com/maxhabra/MetalVoice2/blob/b3ccfc2fc1d21beb065732d2d9c139edb4df8fe8/Sources/AudioCore/AudioCore.cpp#L299)

## 3. AudioController.swift — a transient open error cancels reconnection

**High · traced through start completion and polling.** When a device is missing, the controller keeps the user's running intent and retries. A device can reappear in enumeration before Core Audio can open it. If that first open fails, the completion schedules another retry and immediately clears `wantedRunning`. The poll condition then prevents the scheduled retry. The user has to press Start again. [Start completion](https://github.com/maxhabra/MetalVoice2/blob/b3ccfc2fc1d21beb065732d2d9c139edb4df8fe8/Sources/MetalVoice2/AudioController.swift#L295) · [Retry condition](https://github.com/maxhabra/MetalVoice2/blob/b3ccfc2fc1d21beb065732d2d9c139edb4df8fe8/Sources/MetalVoice2/AudioController.swift#L403)

**Change and test.** Preserve running intent for identifiable transient device-open failures, using a bounded retry interval. Return a typed failure from the native start API so permission, model and unsupported-format errors can take distinct paths. Simulate a selected device disappearing, reappearing, failing the first open and succeeding on the second. Verify that pressing Stop cancels pending recovery.

## 4. Transport.cpp — strong downsampling folds sound into the speech band

**Medium · reproduced offline.** The cutoff follows the rate ratio, but the filter always has 32 taps. Its transition band is too wide at large ratios. A 5 kHz sine sent to an 8 kHz output aliases to 3 kHz. At 48 → 8 kHz, the resulting output was only 14.06 dB below the input. At 192 → 8 kHz, attenuation was just 2.60 dB. Both paths fall within accepted device rates. This issue matters most for low-rate virtual outputs. [Filter construction](https://github.com/maxhabra/MetalVoice2/blob/b3ccfc2fc1d21beb065732d2d9c139edb4df8fe8/Sources/AudioCore/Transport.cpp#L15) · [Render loop](https://github.com/maxhabra/MetalVoice2/blob/b3ccfc2fc1d21beb065732d2d9c139edb4df8fe8/Sources/AudioCore/Transport.cpp#L99) · [Accepted rates](https://github.com/maxhabra/MetalVoice2/blob/b3ccfc2fc1d21beb065732d2d9c139edb4df8fe8/Sources/AudioCore/AudioCore.cpp#L406)

**Change and test.** Use a ratio-dependent filter length or staged decimation for large reductions. Keep the short kernel for normal near-unity clock correction. Adjust priming, the retained filter wing and timestamps for any new delay. Test in-band gain, stopband attenuation and callback p99 at both extreme and common rates. The probe used a half-amplitude sine in 10 ms blocks and compared steady-state RMS after the startup fade.

## 5. localvqe_graph.cpp — each model hop creates a worker

**Medium · measured in the shipped CPU backend.** The wrapper selects two threads without attaching a persistent GGML thread pool. After warm-up, a pthread counter observed 100 worker creations across 100 model hops. At 16 ms per hop this means about 62.5 thread creations per audio second. This runs on the processing worker outside the Core Audio callback, but consumes scheduling headroom. [Backend configuration](https://github.com/maxhabra/MetalVoice2/blob/b3ccfc2fc1d21beb065732d2d9c139edb4df8fe8/ThirdParty/LocalVQE/upstream/ggml/localvqe_graph.cpp#L798) · [Graph execution](https://github.com/maxhabra/MetalVoice2/blob/b3ccfc2fc1d21beb065732d2d9c139edb4df8fe8/ThirdParty/LocalVQE/upstream/ggml/localvqe_graph.cpp#L1595)

**Optimization experiment.** A disposable prototype attached one two-thread pool with polling disabled. Four paced 500-hop runs used stock → pool → pool → stock order. Stock averaged 8.05–8.12 ms per 16 ms hop, the pool 8.12–8.13 ms. Stock p99 ranged from 8.61 to 10.20 ms, the pool from 8.72 to 10.68 ms. Both used about one CPU core. Reuse removes an identifiable source of thread churn, but these runs give no reason to ship it for speed. Profile the graph kernels and history copies under sustained load before changing a pinned dependency. Verify output parity and energy use before adopting a new backend.

**Correctness in the same call.** The graph compute return status is ignored. A backend failure can leave old output available while the caller still copies it and advances recurrent state. Check the status before both operations, propagate a processing error, and inject a failed compute in a test. [Unchecked return](https://github.com/maxhabra/MetalVoice2/blob/b3ccfc2fc1d21beb065732d2d9c139edb4df8fe8/ThirdParty/LocalVQE/upstream/ggml/localvqe_graph.cpp#L1604)

## Release qualification

The final app passed signature verification. A clean copy also passed strict verification. The synced Documents folder adds Finder metadata that prevents strict verification in place. The original v0.9 suite passed before these regressions were added. LocalVQE reached 41.45 ms p99 per 10 ms block in that run. Pool reuse addresses thread churn. Heavy scheduling load can still push inference over the block deadline. A physical hot-plug/sleep-wake test with the selected microphone and virtual output, and a speaker double-talk listening test, remain open. The integration notes also flag an unanswered upstream question about redistribution of the selected RNNoise weights. [Hardware protocol](BASELINE.md) · [Weight provenance](../ThirdParty/RNNoise/README.md#notices-and-model-provenance) · [Upstream issue](https://github.com/xiph/rnnoise/issues/284)

## Reproduce the quality table

The README's published figures come from Table 1 of Rika, Sapir and Gus, using their multilingual low-SNR set. The local check uses the Edinburgh VoiceBank+DEMAND test archives. Its scores serve as a regression baseline for the native adapters. [Paper, accepted version](https://arxiv.org/html/2512.16420v3#S4.T1) · [Edinburgh dataset](https://doi.org/10.7488/ds/2117)

Download `clean_testset_wav.zip`, `noisy_testset_wav.zip` and `logfiles.zip` into `build/quality/`. From the repository root, with Python 3.9 on Apple Silicon:

```sh
./scripts/build.sh
swift build --build-system native -c release --product quality-render
python3 -m venv build/quality/env
build/quality/env/bin/python -m pip install 'numpy==1.26.4' 'scipy==1.13.1' 'Cython==3.0.12' wheel 'pystoi==0.4.1'
build/quality/env/bin/python -m pip install --no-build-isolation 'pesq==0.0.4'
OPENBLAS_NUM_THREADS=1 build/quality/env/bin/python scripts/quality_scores.py \
  --renderer .build/release/quality-render --resources build/MetalVoice2.app/Contents/Resources \
  --clean build/quality/clean_testset_wav.zip --noisy build/quality/noisy_testset_wav.zip \
  --logfiles build/quality/logfiles.zip --csv build/quality/scores.csv
```

The script selects one clip per speaker × noise × SNR cell, choosing the clip ID with the lowest SHA-256 digest in each cell. All nine modes receive identical 48 kHz input, reset per utterance. Each render is zero-flushed through the known signal delay, trimmed to the original length and downsampled to 16 kHz alongside the clean reference. The scores are arithmetic means per utterance, using wideband PESQ and original STOI. The CSV stores STOI on its native 0–1 scale. The README multiplies it by 100. PESQ/STOI do not evaluate treble above 8 kHz here. The dataset supplies neither an independent playback reference for AEC nor matched room-reverberant/dry recordings for WPE. [PESQ implementation](https://github.com/ludlows/PESQ) · [STOI implementation](https://github.com/mpariente/pystoi) · [Per-clip scores](benchmarks/voicebank40.csv)

For timing, the renderer measures each full 480-sample call to the native pipeline, with the model reset between utterances. It includes warm-up frames and OS scheduling. File I/O, initialization, resets, terminal padding, transport and the calling app are outside the timer. A p99 below 10 ms still permits individual blocks to miss their deadline.

The reported run used an Apple M1 Max, macOS 27.0.1 and Python 3.9.6. The archived local scores cover 360 mode/utterance pairs. Downloaded archive SHA-256 digests follow.

| File | SHA-256 |
|---|---|
| clean_testset_wav.zip | `b60bfc2b9df38466d8eb45057c087c6e5dee196c67a0ed7bca628ae2a8611c4e` |
| noisy_testset_wav.zip | `ba4a893f45b627f15209ba30ab584dd495b3680cd6f216dd08d143a257611993` |
| logfiles.zip | `ab37c41f31bc2b6c204cc10c8e5bb8bde800307fdb49f277e366d986fefc9e39` |
