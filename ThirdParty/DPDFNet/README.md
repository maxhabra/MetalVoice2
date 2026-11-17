# DPDFNet-2 48 kHz HR

Official source: https://github.com/ceva-ip/DPDFNet at
`9bd9844a227bb6aa57e55588d8d0e961fcff1c46`.
Official models: https://huggingface.co/Ceva-IP/DPDFNet at
`dd6818d00f50c836fed43a6243ebe49116de5964`.
Code and weights are Apache-2.0. The unmodified upstream license and the source
manifest accompany the compiled model in the app's Licenses directory.

The selected model is `dpdfnet2_48khz_hr`, with 2.583 million parameters.
The checked-in ONNX file is the independent reference; the checkpoint and
unmodified streaming Python source are only used to generate the Core ML model.
No Python, ONNX Runtime, download, or background service ships with this backend.

`scripts/export_dpdfnet_coreml.py` converts the upstream streaming wrapper to a
Float32 Core ML graph. Explicit state has 56,436 elements, including the exact
empirical normalization initialization from the official ONNX metadata. Four
bidirectional GRUs run across frequency, never future time. Their gate
projections are fused and the fixed frequency sequence unrolled to avoid generic
converter loop overhead; PyTorch's reset-after equation is preserved. No
quantization, additional gate, postfilter, attenuation limit or model retraining
is applied. Model inputs/outputs and recurrent state remain Float32.

One prediction consumes each 480-sample hop at 48 kHz. Native Accelerate DFT
uses the upstream 960-sample Vorbis window, raw FFT scaling, conjugate spectrum,
inverse scaling and overlap-add. The first callback emits a silent hop while
the initial full analysis window accumulates. The model's magnitude-mask buffer
delays spectra by two frames and its deep-filter stage aligns them two frames
further back. Along with analysis/synthesis alignment, the measured signal delay
is 2,400 samples / **50 ms**, not 10 or 20 ms. WebRTC adds 334 samples.

Creation, specialization, warm-up, prediction, state resets and destruction all
run on the existing serial suppression worker. Hardware callbacks do not call
Core ML. Input arrays and DSP buffers are reused; Core ML may allocate temporary
worker memory. Only the selected enhanced backend is loaded. A resource/load
failure fails Start instead of silently passing an unsuppressed microphone.

The shipped path explicitly selects Core ML CPU execution, qualified on the M1
Max. Automatic accelerator placement missed the 10 ms processing deadline during
qualification; the CPU path completed around 1.2 ms per hop. GPU/ANE/all-device
configurations remain developer test options, not the default. Details reports
**Core ML · CPU**, an actual compute constraint. Expensive graph inspection is
excluded from stream setup. This is not a claim of GPU or Neural Engine offload. This model has a higher
processing cost and longer signal delay than the optimized regular DFN3.

Regenerate with an isolated build environment containing
`scripts/requirements-dpdfnet.txt`, then run:

```sh
python scripts/export_dpdfnet_coreml.py
bash scripts/build_dpdfnet.sh
```

`scripts/verify_dpdfnet.py` checks source, weights, exporter, model package,
initial state and fixture hashes. `scripts/check_dpdfnet_reference.py` compares
spectra and complete audio against official ONNX inference and independently
measures signal alignment. It requires `build/libDPDFTest.dylib`, generated from
`Sources/AppleDF/DPDF.mm`, and generated speech at `build/deepfilter-speech.f32`.
No microphone is used. `dpdf-tests` runs without Python using a five-second
reference fixture, and additionally checks every WebRTC level, resets,
switching, malformed buffers, resource failures and latency reporting.
