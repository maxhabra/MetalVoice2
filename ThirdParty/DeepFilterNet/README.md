# DeepFilterNet3 native and Core ML integration

Uses upstream [DeepFilterNet](https://github.com/Rikorose/DeepFilterNet) commit
`7b14b895fe7fc42cab72b4a4c1e49c982bd6404b` (2026-11-06) and its regular
`models/DeepFilterNet3_onnx.tar.gz`. Source and model bytes are unmodified and
verified by `scripts/verify_deepfilter.py` against `SOURCE_MANIFEST.json`.
The low-latency variant is not selected or bundled. Version 0.5 uses a verified,
full-precision Core ML export by default and retains the unmodified native
runtime for fallback and reference comparisons.

## Native runtime and build

`reference/Cargo.toml` is an integration manifest compiling upstream libDF's
Rust sources with the inference/transform features only. It omits unrelated
Python bindings, codec support, datasets, training, and command-line tools.
The original upstream manifests and lockfile remain available under `upstream`.
The application's dependency graph is pinned in `bridge/Cargo.lock`; Tract is
0.21.4, matching the inspected upstream lockfile. Compiler/model/graph changes
require repeating the reference and alignment checks.

`scripts/build_deepfilter.sh` builds the bridge as a static library under
`build/deepfilter`, using repo-local Cargo caches and macOS 15 deployment flags
for Rust and Tract's native ARM kernels. Building requires Rust/Cargo (tested
with 1.98.1), and the first build downloads locked crates. Application startup
does not download anything or launch an external process. Tract is compiled
into the native executable; Core ML is Apple's system framework. No Python or
external ONNX Runtime ships with the application.

`Resources/DeepFilterNet3.mlpackage` contains the converted model. Its source
archive, exported files and converter script are pinned in
`Resources/DeepFilterNet3.manifest.json`. `scripts/build_coreml.sh` verifies and
compiles it with Apple's Core ML compiler, caching the compiled resource until
the manifest changes. The ordinary build needs no Python ML packages.

To regenerate the model, create an isolated environment from
`scripts/requirements-coreml.txt` and run:

```sh
python scripts/export_deepfilter_coreml.py Resources/DeepFilterNet3.mlpackage --skip-inactive-decoders
```

The exporter writes contract/state metadata beside the package. Refresh the
manifest hashes only after upstream ONNX and complete audio parity tests pass.
The model keeps all arithmetic and recurrent history in Float32. Pure and
mixed Float16 candidates altered local-SNR threshold decisions on the speech
fixture; they are experiments, not the shipped model. Apple's macOS 15 MLState
export supports Float16 state, so this model instead uses an explicit Float32
history tensor with a reused native input buffer.

## Core ML execution

`Sources/AppleDF/AppleDF.mm` loads the bundled compiled model on the suppression
worker with `MLComputeUnitsAll` and fast-prediction specialization. It warms
prediction and clears all history before capture starts. Input arrays and
feature providers are reused; output strides/data types are handled explicitly,
and each call has an autorelease pool. No batch accumulation or extra frame
queue is introduced.

One prediction runs the encoder, then conditionally executes the two decoders.
Temporal convolution caches and all five GRU states persist between calls.
Skipped decoder branches retain their history exactly as upstream does. The
native DSP adapter in `bridge/src/stream.rs` calls upstream's actual FFT/window,
ERB construction, normalization, interpolation and synthesis functions and
uses a bounded five-frame spectrum ring for masking/deep filtering. A Rust
oracle test verifies this assembly sample-for-sample against full `DfTract`.

Core ML chooses a supported device for each operation. The tested M1 Max
prefers CPU for this single-frame, full-precision graph; permitting all compute
units is not evidence of GPU or Neural Engine execution. Details and JSON
reports expose the preferred-device mask from `MLComputePlan` (CPU=1, GPU=2,
Neural Engine=4), which is a plan, not utilization telemetry. CPU work and
prediction times are measured separately. Loading failure releases the Core ML
state and uses the native reference, explicitly reported as a fallback.

## Processing contract

`bridge/src/lib.rs` exposes the native reference C ABI; `stream.rs` exposes the
DSP/predictor interface. Both are used by `NoisePipeline.cpp`:

- Exactly 480 normalized Float32 mono samples at 48 kHz, with disjoint input and
  output buffers. RNNoise's FloatS16 scale must not be used with this backend.
- Both paths call upstream's streaming STFT/ISTFT, Vorbis window, ERB
  construction, feature normalization and mask interpolation. The native path
  uses full libDF inference; the optimized path exports the same weights,
  convolution/GRU equations and conditional state transitions into Core ML.
  Its small spectrum-ring/deep-filter adapter is verified against full libDF.
- Uses `RuntimeParams::default_with_ch(1)`: full suppression, no added postfilter,
  no custom gain or VAD gate. Upstream's local-SNR-dependent stages/silence
  optimization are retained. Synthetic stationary tones may be removed, so
  they are not a valid voice-preservation test.
- Creation, warm-up, inference, reset, and destruction all run on the same
  dedicated worker: upstream Tract's state is not Rust `Send` and is never
  transferred from the control queue. Startup waits for the worker's readiness
  before starting hardware capture. Creation warms inference, then restores
  pristine state. Native reset clones the pristine runtime; Core ML reset zeros
  its explicit history while the DSP reset restores pristine spectrum/overlap
  and normalization state. Calling upstream `init()` alone does not reset all
  of its fields.
- Rust panics/errors are caught at the C boundary. Failed inference returns an
  error and silences that block; the audio engine stops/reconnects through its
  existing error path. No panic unwinds into Swift/C++.

DeepFilterNet3 is an alternative to RNNoise, not an additional neural stage.
Optional WebRTC runs first on the same worker and frame; no intermediate
queue is added. Only the selected neural processor/runtime has active state.
The fallback model constants remain in the executable but are not loaded into
a Tract inference session when Core ML is active.

## Delay, allocations, and verification

The pinned model has a 960-sample FFT, 480-sample hop, and two lookahead hops:
`960 - 480 + 2 * 480 = 1440 samples`, or **30 ms**. With WebRTC the alignment
delay is 1774 samples / 36.9583 ms. Frame accumulation, worker queue margin,
device/transport timing, and receiving-app buffering are separate. The app's
timestamp counter includes the selected stages' alignment delay.

Rust tests verify C-bridge output against a separately created upstream runtime,
including repeated reset. Delay tests use upstream's small attenuation-limit
mix to expose original-signal alignment across frame offsets without confusing
neural phase changes with pipeline buffering. This test-only limit is not the
production setting. Native C++ tests additionally check the complete WebRTC →
DFN3 chain at every WebRTC strength, normalization, switching, malformed input,
and sustained processing. Core ML tests additionally compare the entire audio
pipeline at all WebRTC levels, resets, silence/recovery, switching, malformed
input and failed-model fallback. The speech fixture is optional and local;
these tests never start capture. `--df-backend native` on the app's live
benchmark selects the reference for a controlled comparison.

Core ML and Tract/libDF can allocate during inference. That work stays on the
existing dedicated worker; hardware audio callbacks remain free of model
execution and allocation. WebRTC/RNNoise's allocation-free test remains
separate. This implementation does not claim allocation-free Core ML calls or
direct custom Metal kernels. Faster inference does not remove the 30 ms model
alignment delay or the transport's separately validated worker headroom.

## Licenses

Upstream explicitly licenses code and model weights under MIT or Apache-2.0;
the original license files are included. `DEPENDENCIES.json` and
`DEPENDENCY-NOTICES.txt` retain the resolved crates' declarations and available
notices, including build-time dependencies. The published crunchy and realfft
crates provide their MIT declarations in Cargo metadata without a separate
license file; those declarations/authors are retained. The Rust standard
library notices used for this build are in `RUST-STDLIB-NOTICES.html`.
Notices and manifests are copied into the application bundle.
