# RNNoise native integration

Source: [Xiph RNNoise](https://github.com/xiph/rnnoise), pinned to
`70f1d256acd4b34a572f999a05c87bf00b67730d` (2025-02-22, official GitHub mirror).
The upstream source and selected generated model files remain byte-for-byte unchanged.
`SOURCE_MANIFEST.json` records Git blob/SHA-256 hashes and archive provenance;
`scripts/verify_rnnoise.py` verifies them during builds and tests.

## Model and build

Uses the **regular** official model identified by upstream `model_version`:
`0a8755f8e2d834eff6a54714ecc7d75f9932e845df35f8b59bc52a7cfe6e8b37`.
The archive at `https://media.xiph.org/rnnoise/models/rnnoise_data-<hash>.tar.gz`
was verified against that SHA-256 before selecting only `src/rnnoise_data.c`
and `src/rnnoise_data.h`. The training checkpoints and alternate model are not bundled.
The large generated C tables are compiled into native read-only model data;
their text size is not the application size or resident memory.

SwiftPM builds the upstream inference source list directly, statically linked.
On Apple Silicon, upstream `vec.h` selects the ARM NEON implementation. No
fast-math override, custom model conversion, Python runtime, Metal/Core ML
runtime, or runtime download is involved. Model state exists only when RNNoise
is selected and the audio route is active. The model constants are part of the binary.

## Audio contract

`Sources/AudioCore/NoisePipeline.cpp` owns optional WebRTC then optional RNNoise,
with exactly 480 mono samples at 48 kHz per call. A single processing worker
feeds both stages without an intermediate queue. The original transport bypass
is used when both are off. Settings change only while the route is stopped.

RNNoise expects FloatS16 amplitudes. The adapter converts normalized Core Audio
floats to that range, runs in place, then converts back. It bounds input/output
and handles non-finite input. It uses the published size/init/destroy API with an
explicit allocation check instead of upstream's unchecked allocation in create.
Reinitializing the existing RNNoise state clears history after capture gaps.

This pinned version has **960 samples / 20 ms signal delay**: analysis/synthesis
overlap plus a delayed-spectrum frame. The worker subtracts that delay from
output-origin timestamps. Combined with WebRTC, the total is 1294 samples /
26.9583 ms, independently of frame accumulation, worker headroom, transport,
and device delays. Tests verify individual impulse peaks across frame offsets
and broadband correlation through the cascade at every WebRTC strength.
An isolated impulse through both nonlinear processors may have its largest
peak in a later filter tail; it is not a reliable cascade-delay probe.
This is not a claim of 20 ms total app latency.

RNNoise's VAD result does not gate the microphone. There is no invented
aggressiveness slider or extra gain stage. Audio remains fully local.

## Notices and model provenance

Source licensing is documented by upstream `COPYING` (BSD-3-Clause) and the
individual source notices. `LICENSE-NOTICES.txt` retains the source copyright
and license comments, including inherited BSD-style DSP/neural code notices.
These notices, the manifest, and this document are copied into the app bundle.

The separately distributed current model archive has no explicit model-license
file. As checked on 2026-11-17, [upstream issue 284](https://github.com/xiph/rnnoise/issues/284)
still requests clarification and has no maintainer answer. The source license
must not be presented as a confirmed separate license grant for these weights.
This local prototype uses the upstream-distributed model; resolve its redistribution
terms or select a model with explicit terms before a public/commercial release.
No release or distribution approval is implied by this integration.
