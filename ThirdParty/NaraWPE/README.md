# Room reverberation reduction

Reference: [NARA-WPE](https://github.com/fgnt/nara_wpe), commit
`5fbfc3639274e5a70dffcf08e20615a38c372ebb`. MIT, copyright 2018
Communications Engineering Group, Paderborn University. `wpe.py` is an unmodified
reference used only to generate test fixtures; Python does not ship in the app.
The license and provenance are included in the app bundle.

`Sources/AudioCore/RoomDereverb.cpp` adapts the frame-online weighted-prediction
error update from `online_wpe_step` to native single-channel processing. This is
an experimental room de-reverberation control, **not acoustic echo cancellation
of Mac speaker playback**. It needs no system audio tap or additional permission.

Configuration:

- 48 kHz mono, 480-sample hop, 960-sample sine window and normalized overlap-add.
  Accelerate performs the FFTs. Startup uses a zero-padded prior hop, so the
  complete input (including the first samples) is retained with 480 samples /
  10 ms signal delay. The 30 ms prediction gap is historical context, not extra
  lookahead or buffering.
- 12 complex prediction taps per frequency bin, starting 3 hops in the past;
  inverse covariance initially identity, forgetting factor 0.999. Smoothed
  observation power (0.9 previous + 0.1 current) weights the adaptive updates.
- Double precision for prediction/covariance and Float32 FFT/audio. Updates
  preserve Hermitian symmetry; inactive bins freeze adaptation below power
  1e-6. Invalid/divergent state resets that frequency bin. Input NaN/Inf fails
  closed and resets all history.
- A conservative residual magnitude mask retains input phase, avoids spectral
  boosts, and caps attenuation at about 8 dB. Fast mask opening protects onsets.
  This adaptation is intentionally gentler than emitting the raw WPE residual.
  It does not guarantee recovery of the original dry voice; early reflections,
  short clips and moving speakers may see limited benefit. Speech tone can
  change. Off by default; compare by ear in the actual room.
- Runs **before** WebRTC and the selected neural stage on their existing worker,
  with no additional queue. All state is allocated during setup; streaming and
  reset allocate nothing. No trained model, GPU dispatch or separate service.

`generate_room_fixture.py` uses the pinned upstream update plus an independent
NumPy STFT and mask to generate the reference. `room-tests` checks that output,
startup alignment, repeatable resets, malformed input, long-stream stability,
allocation behavior, all 20 combinations of WebRTC/enhanced modes and reported
latency. Quality probes use synthetic room responses; they do not establish
perceptual performance in the user's room.
