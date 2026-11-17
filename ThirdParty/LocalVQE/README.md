# LocalVQE joint v1.3 integration

Sources: [LocalVQE](https://github.com/localai-org/LocalVQE/tree/7f61cd05095d42e56a135b303f5bb4d761da22b9), commit `7f61cd05095d42e56a135b303f5bb4d761da22b9`; its [GGML submodule](https://github.com/ggml-org/ggml/tree/c044a8eeae2591faa0950c8b5e514cbc4bbfc4ca), commit `c044a8eeae2591faa0950c8b5e514cbc4bbfc4ca`. The unused GGML MNIST demo ignore file is amended to retain its committed files; DSP sources are unchanged. Source archives and every vendored file are hashed in `SOURCE_MANIFEST.json`; `scripts/verify_localvqe.py` checks them before building.

Model: [published joint v1.3, 4.8M parameters, Float32 GGUF](https://huggingface.co/LocalAI-io/LocalVQE). SHA-256 `c4f7912485c32cfc206c536f2f050b52513f2f613fdbc616391f6b26ab1d51ec`, 19,268,160 bytes. Code and published model are Apache-2.0; GGML is MIT. Both licenses accompany the app. The GGML library is statically linked, with native ARM kernels and Accelerate enabled. This build uses CPU inference, not Metal or Core ML. No Python/server runs in the app.

The local CMake file builds the unmodified joint-model C API/graph sources, common utilities and DAF implementation with GGML's CPU backend. Other architectures, command-line audio I/O, CUDA/Vulkan/Metal, OpenMP and the separate BLAS backend are excluded. Consequently the upstream GRU patch needed only by the unused GTCRN backend is not applied. CPU-backend Accelerate support remains enabled. Tests also build the upstream regression executable. CMake 3.31.10 and Ninja 1.13.0 are isolated under `build/localvqe-env`.

## Behavior

Enhanced → LocalVQE selects joint **noise + room cleanup**. Its far-end reference is deliberately zero, following upstream's no-sidechain usage. The optional upstream hard noise gate is explicitly disabled. This mode does **not** perform reference-based speaker echo cancellation: no playback tap is opened. Separate AEC3 and WPE are suspended regardless of native setter order, and both UI checkboxes are disabled and unchecked. Previous selections are kept and restored when another Enhanced mode is selected. WebRTC may still run before LocalVQE.

The model's internal rate is 16 kHz mono with 256-sample hops (16 ms); it is not full-band 48 kHz. The 48 kHz device pipeline stays intact. `LocalVQEAdapter` uses causal 193-tap Kaiser FIRs (7.2 kHz cutoff, beta 8.6), factor-three decimation and polyphase interpolation. A fixed-size FIFO reconciles the 10 ms host blocks and 16 ms model hops. No allocation is performed by this adapter. GGML inference may allocate on the existing suppression worker, never in Core Audio callbacks. Only the selected model is loaded; start/reset/stop own all model state on that worker.

Total alignment delay at 48 kHz is **1,632 samples / 34 ms**:

- 672 samples / 14 ms fixed framing margin: `(256 - gcd(256,160)) * 3`.
- 768 samples / 16 ms causal analysis/synthesis overlap-add.
- 192 samples / 4 ms combined resampler group delay.

This delay is included in the source timestamps, Details and reports. It excludes transport/device/calling-app buffering. All WebRTC levels add their usual 334 samples. Pipeline and engine suppress the separate WPE/AEC delays for LocalVQE.

Two CPU threads are the default after measuring 1/2/4 on M1 Max. One took about 13 ms/model hop; two about 7.3 ms; four about 4.4 ms but used more total CPU. The complete two-thread adapter/model path measured about 4.7 ms average per 10 ms host block (some blocks need no inference), around 93% of one core offline. This model is heavier than the app's other options. These are processing-only measurements, not power or live end-to-end results.

## Verification

`localvqe-regression` compares to the upstream committed v1.3 output fixture. `localvqe-tests` exercises every resampling/framing phase, passband and alias rejection, ten minutes of simulated adapter FIFO wrap, waveform timing, model reset, silent input, malformed input, all five WebRTC choices, WPE exclusion, setting restoration and native delay accounting. See `docs/VERIFICATION.md` for the final run and live-test limits. Listening superiority over Amazon Voice Focus or another model is not established by these numerical tests.
