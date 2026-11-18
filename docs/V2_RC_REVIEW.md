# v2.0 release candidate review

## Changes

| Path reviewed | Finding and action |
|---|---|
| [Voice worker](../Resources/RVC/worker.py) | ContentVec and RMVPE run sequentially, followed by the PyTorch decoder. Stop ONNX worker threads spinning between inference calls. Reuse crossfade and search windows, skip interpolation for 48 kHz decoders and write contiguous float32 audio directly to the pipe. |
| [Core Audio callbacks and diagnostics](../Sources/AudioCore/AudioCore.cpp) | Apply the per-sample mute ramp only during transitions. At steady gain the render callback measures the peak or clears the muted block. Compute fixed cleanup delay once per stream and stop scanning the latency histogram as soon as p95 is found. |
| [Voice transport](../Sources/AudioCore/include/RVCStream.hpp) | Reviewed chunk buffering, timestamp propagation, mute and worker shutdown. The fixed playout reserve and short socket transfers are follow-up items below. |
| [Audio controller](../Sources/MetalVoice2/AudioController.swift) and [cleanup pipeline](../Sources/AudioCore/NoisePipeline.cpp) | Reviewed startup, route recovery, processor order and metering. Stream setup runs on the control queue, model inference runs outside the Core Audio callbacks, and the UI meter pauses when the panel closes. Corrected the sample-rate error to cover voice-only operation. |
| [Popover help](../Sources/MetalVoice2/PanelHelp.swift) | Pointer movement previously created and invalidated a timer for every mouse-move event. Moving the deadline on one pending timer preserves the dwell time and disabled-control help. |

## Offline voice benchmark

M1 Max, one pitch-guided RVC v2 model with a 32 kHz decoder, RMVPE on CPU, pitch +10, 100 Hz high-pass, 10 kHz low-pass, gate off and no retrieval index. Each run warmed up the model, cleared its audio history, reset the excitation seed, and processed 12 synthetic voiced blocks. Blocks arrived at the selected chunk interval when inference could keep up. Each range covers two runs of the same revision.

| Chunk / extra | Median inference before → after | Worker CPU time per chunk before → after |
|---|---:|---:|
| 500 / 340 ms | 259–262 → 256–258 ms | 621–622 → 600–603 ms |
| 100 / 80 ms | 110–111 → 105–108 ms | 245–249 → 223–226 ms |

CPU time sums all worker threads. Both optimized runs produced sample-identical output to the original at both settings. The 500 ms chunks had compute headroom. At 100 ms even the median missed the deadline. These short worker-only measurements exclude cleanup, capture and playout buffering. Live call latency, sustained scheduling load and listening quality need separate measurements.

## Verification

- The [offline suite](../scripts/test.sh) passed on the M1 Max, including cleanup model reference tests, routing and bypass, the 128-sample mute/unmute fade, device recovery, saved presets and gate hold. A sandboxed run failed the DeepFilterNet3 Core ML parity check after an IOSurface access error. The full suite passed with hardware access.
- The packaged worker loaded the real 32 kHz model and returned the expected sample count at 50/0, 500/340 and 1000/1500 ms Chunk/Extra. Each case started with gated silence and produced finite, nonzero output for voiced input.
- A separate popover test exercised hover help, tab switching, Details, menu icon clicks, Escape and outside-click dismissal.

## Follow-up items

- **Playout reserve.** The bridge counts 12 processing ticks with output available before starting playout. At the normal 10 ms cadence this is about 120 ms. Measure queue depth and underruns under call load before tuning it. Short socket transfers currently end the stream. A bounded carry buffer could handle recoverable partial transfers while preserving sample order.
- **Model compatibility.** The worker calls the decoder’s encoder, flow and synthesis layers directly, uses speaker ID 0, and removes a deprecated PyTorch weight-normalization wrapper at load time. Keep seeded audio fixtures for v1 and for 40/48 kHz decoders before changing those calls or upgrading PyTorch. The 48 kHz interpolation shortcut also needs an end-to-end fixture with a real 48 kHz checkpoint.
- **Index import.** The worker checks feature width and entry count after FAISS opens the file. Those checks cannot establish who produced the index or whether it belongs to this speaker. Model import hardening and a paired-index regression fixture merit a separate change.
- **Setup and diagnostics.** The optional Python environment and two shared ONNX assets are separate installs. Applying a preset restarts the worker and warms the models again. The live report measures cleanup work and total audio-path latency. Separate per-stage RVC timings would help distinguish model compute from buffering in longer trials.
