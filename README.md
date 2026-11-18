<h1>
  <a href="Resources/Brand/MetalVoice2-logo.png">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="docs/assets/MetalVoice2-logo-on-white.jpg">
      <img src="Resources/Brand/MetalVoice2-logo.png" width="360" alt="MetalVoice2">
    </picture>
  </a>
</h1>

<a href="Resources/Brand/AppIcon.icns"><img align="right" src="Resources/Brand/AppIcon.png" width="88" alt="MetalVoice2 macOS app icon"></a>

A native macOS menu bar app that cleans up your microphone and sends it to a virtual microphone for calls or recording. Your voice keeps its original pitch. All processing runs locally, and the app saves no audio.

[App icon (ICNS)](Resources/Brand/AppIcon.icns) · [App icon (PNG)](Resources/Brand/AppIcon.png) · [Full logo (PNG)](Resources/Brand/MetalVoice2-logo.png)

## Build and use

**A virtual audio driver is required** to send the cleaned microphone signal to your calling or recording app. Install either [BlackHole 2ch](https://github.com/ExistentialAudio/BlackHole) or [VB-CABLE for macOS](https://vb-audio.com/Cable/).

MetalVoice2 runs on Apple Silicon with macOS 15 or later. To build from source, install Command Line Tools with Swift 6, Rust/Cargo and Python 3. The first build needs internet access for build dependencies.

```sh
./scripts/build.sh
open build/MetalVoice2.app --args --show
```

1. In MetalVoice2, choose your physical microphone and its input channel. Select the installed virtual driver as the output. Set the physical microphone to **48,000 Hz** in Audio MIDI Setup to use the filters.
2. Press **Start** and allow microphone access. In your calling or recording app, select that driver's recording device as the microphone. With BlackHole, select **BlackHole 2ch** in both apps. Switch off the calling app's own filtering when comparing models.
3. Use the menu bar icon to change settings. Right-click for Start/Stop, Mute and Quit. Closing the panel keeps the microphone running.

## Choose a processor

Processing order is **Echo → Room de-reverb → WebRTC → Enhanced**. Each stage is optional. Select one Enhanced model at a time. Delay is the signal delay added by that stage at 48 kHz. Compute p99 is how long 99% of the measured 10 ms blocks took to process on an M1 Max.

| Control | What it does | Added delay | Compute p99 |
|---|---|---:|---:|
| Echo · WebRTC AEC3 | Removes Mac speaker playback picked up by the mic, using a stereo playback reference | 8.96 ms¹ | 0.24–0.38 ms² |
| Room de-reverb · WPE | Softens late reflections of your own voice using the mic alone | 10 ms | 0.68 ms² |
| WebRTC · Low to Very high | Low-cost noise suppression with four strengths | 6.96 ms | 0.010–0.045 ms |
| RNNoise | Lightweight recurrent speech denoiser, native ARM CPU | 20 ms | 4.42 ms |
| DeepFilterNet3 | Full-band neural noise suppression, Core ML with native fallback | 30 ms | 2.41 ms |
| DPDFNet-2 | Full-band dual-path denoiser, 48 kHz HR model on Core ML CPU | 50 ms | 4.43 ms |
| LocalVQE | Joint noise and room cleanup, 16 kHz speech, two GGML CPU threads | 34 ms | 31.08 ms³ |

¹ Echo also reserves one playback tap buffer. Enable **System Audio Recording** when prompted and send the calling app's playback to the Mac's default physical output. LocalVQE disables Echo and Room de-reverb while selected and restores their settings when you switch back. Its playback reference is silent, so use AEC3 when you need speaker echo cancellation.

² AEC3 and WPE timings come from their respective synthetic tests. The other timings come from the post-fix offline speech run below. They include OS scheduling. ³ LocalVQE exceeded the 10 ms processing budget at p99 in this run. An earlier dedicated test measured 8.06 ms p99. Scheduling load matters for this model. [Timing protocol](docs/V1_REVIEW.md#reproduce-the-quality-table) · [Earlier measurements](docs/VERIFICATION.md)

**Audio path latency** measures capture to scheduled virtual output using Core Audio timestamps. It includes enabled filters and transport buffering. The receiving app and network add their own delay. **Details → Copy report** includes device rates, queue depth, underruns and processing time.

## Speech quality and intelligibility

**Published research.** Rika, Sapir and Gus tested these models on the same 324 recordings spanning 12 languages and nine noisy environments at 0, 5 and 10 dB SNR. These are the authors' results from their multilingual test set. Higher PESQ means better predicted speech quality. Higher STOI means better predicted intelligibility. STOI is shown on a 0–100 scale. [DPDFNet paper, Table 1, accepted version](https://arxiv.org/html/2512.16420v3#S4.T1)

| Model in the paper | PESQ ↑ | STOI × 100 ↑ |
|---|---:|---:|
| Noisy input | 1.32 | 83.2 |
| RNNoise | 2.36 | 88.5 |
| DeepFilterNet3 | 2.76 | 90.5 |
| DPDFNet-2 | **3.14** | **92.6** |

**MetalVoice2 measurements.** Each native mode processed the same 40 held-out VoiceBank+DEMAND utterances, balanced across two speakers, five noises and four SNRs. Each clip starts with fresh model state. Compare rows *within* each table because the recordings and model variants differ. This local test measures additive noise suppression. Room reflections and playback echo need different test signals. [Method and reproduction](docs/V1_REVIEW.md#reproduce-the-quality-table) · [Per-clip results](docs/benchmarks/voicebank40.csv)

| Native mode, used alone | Wideband PESQ ↑ | STOI × 100 ↑ |
|---|---:|---:|
| Noisy input | 1.97 | 92.1 |
| WebRTC Low | 2.36 | 91.5 |
| WebRTC Medium | 2.45 | 90.4 |
| WebRTC High | 2.41 | 89.2 |
| WebRTC Very high | 2.35 | 88.3 |
| RNNoise | 2.14 | 88.0 |
| DeepFilterNet3 | 2.11 | 85.1 |
| DPDFNet-2 | **2.99** | **93.4** |
| LocalVQE | 2.30 | 91.4 |

**WebRTC Medium** is a good starting point for low delay. **DPDFNet-2** led both local quality measures and adds 50 ms of signal delay. Stronger WebRTC settings reduced intelligibility on this sample. Use **Room de-reverb** or try **LocalVQE** if your main complaint is a hollow room sound.

## Verification

Run `./scripts/test.sh` for the offline suite. For a live 60-second comparison using BlackHole 2ch, grant microphone permission, quit the menu app, then run `./scripts/benchmark.sh 60 off --enhanced dpdfnet2`. The script writes counters under `build/benchmarks/`. Local builds are ad-hoc signed. [Five-file release review](docs/V1_REVIEW.md) · [Hardware test guide](docs/BASELINE.md)

MetalVoice2 embeds the official RNNoise pretrained weights. Their redistribution terms remain an open question upstream. [RNNoise model provenance](ThirdParty/RNNoise/README.md#notices-and-model-provenance) · [Upstream licensing question](https://github.com/xiph/rnnoise/issues/284)
