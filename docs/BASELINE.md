# Baseline verification

This protocol was introduced for the transport-only baseline. Version 0.8 provides independent Echo (AEC3) and Room de-reverb (WPE) checkboxes and provides WebRTC and a choice of RNNoise, Core ML optimized DeepFilterNet3 or DPDFNet-2 48 kHz HR; uncheck Echo and Room de-reverb and set both WebRTC and Enhanced to Off for the original pass-through comparison. Live AEC qualification, launch at login, recording, and a bundled virtual microphone remain later work. DFN3 allows all Core ML compute units but may prefer CPU. DPDFNet-2 explicitly uses the CPU configuration qualified on the M1 Max; its signal delay is 50 ms. Record the engine/compute constraint separately from measured CPU time or hardware utilization.

## Controlled baseline

1. Keep the same physical microphone, channel, virtual device, and receiving app across runs. Close voice-changer so it is not also writing to BlackHole.
2. Build the release app with `./scripts/build.sh`. Start it through the `.app` bundle so the microphone usage description is available to macOS.
3. Press Start. Grant microphone access in the macOS prompt. Check that both input and output meters respond to speech and that the receiving app gets audio through BlackHole. Avoid speaker monitoring without headphones.
4. Leave the stream running for at least 60 seconds. Copy the JSON report. Record release revision, device names/rates/buffers, run duration, timestamp latency p50/p95/max, underruns, overruns, and resynchronizations.
5. Check Activity Monitor with the popover closed. Record CPU and memory separately from latency. Repeat while running representative CPU/GPU work.
6. Mute: receiving audio should fade to silence while capture and counters continue. Unmute: audio should return. Stop: capture should end. Quit: microphone access should end.

For a voice-changer comparison, keep conversion in passthrough, record the exact sup1/sup2 state, and compare the same route. Uncheck Echo and Room de-reverb and turn both selectors off for the original transport-only baseline. Compare WebRTC-only, each neural backend alone, and WebRTC followed by each neural backend using identical input and delay-aligned/loudness-matched listening samples when available. RNNoise, DeepFilterNet3 and DPDFNet-2 are alternatives; they do not run together. The WebRTC option is a Sup1 counterpart, not Amazon Voice Focus/Sup2; assess sound-cleanup quality separately from transport and resource measurements. The app does not save microphone recordings automatically.

## Counter interpretation

- Current interval: scheduled output host time minus the resampler's corresponding captured-source host time.
- p50/p95/max: all valid callback samples since Start, including startup after priming. Histograms have 0.1 ms resolution. This is a callback-weighted distribution, not a per-sample distribution.
- Queue: input-sample lead before an output render, in milliseconds. Pass-through target is based on two device blocks or 5 ms, whichever is larger, plus filter support. Any enabled suppression accounts for 480-sample production blocks and reserves one additional 10 ms block for worker scheduling.
- Clock correction: the resampler's adjustment, in parts per million; not an independently measured hardware clock offset.
- Underrun: insufficient source samples for a complete output block after priming. The block becomes silence and the queue reprimes.
- Overrun: a producer block could not fit. It is rejected without overwriting unread data; the consumer resynchronizes.
- Resynchronization: dropped input or excessive queue age caused buffered audio to be discarded and primed again.

Latency is unavailable when timestamps are invalid, the queue is priming, or a discontinuity interrupts output. The engine reports device latency metadata separately through its C API; it is not added to timestamp latency because doing so could double-count portions of the device timeline.

For actual capture-to-receiver latency, a later loopback harness must observe BlackHole from a separate receiving client and correlate a known broadband signal. For physical latency, use a wired hardware loopback. Do not treat this version's timestamp interval as either measurement.

## Recovery checks

While running, unplug/reconnect the selected USB microphone, sleep/wake the Mac, and change a selected device's sample rate or buffer size. The app should stop the old route and reconnect the same saved UIDs. Input/output stalls are detected by the control timer. It must not select a different physical microphone automatically or grow its buffers indefinitely. Permission refusal must produce a useful error; it must not begin capture.

The engine is limited to 8–192 kHz, up to 64 physical input channels, and device buffers small enough for its fixed queue. It maps the selected input channel to the first one or two virtual output channels. These are explicit baseline limits.

## Automated checks

`./scripts/test.sh` runs the Rust reference, C++ transport, and suppression integration tests without opening audio devices. Streaming simulations cover equal rates, 44.1 ↔ 48 kHz, independent-clock drift in both directions, and recovery boundaries. Tests advance simulated audio time; a 120-second simulation is not a 120-second hardware soak.

An eight-hour physical run, receiving-app compatibility, energy profiling, and physical loopback remain qualification work; record those results only after performing them.

## Room de-reverberation and panel checks

The Room de-reverb checkbox reduces room reverberation. The separate Echo checkbox cancels speaker playback. Start
with it off, keep other settings unchanged, then compare speech and word endings
with it on. Leave time for the adaptive filter to settle. It adds 10 ms to the
reported chain delay. Listen for voice-tone changes as well as reduced tails.
The default remains Off; automated room responses cannot qualify your own room.

Details should add a lower section without shifting the header or controls.
Closing it should remove the section and blank space. Check reopening the
popover, long device names, both colour schemes, and a short display where the
panel must scroll. The ordinary collapsed content is about 460 × 290 points.


## Speaker echo live qualification (v0.8; pending)

1. Quit the previous build and reopen `build/MetalVoice2.app`. Enable Echo, Start and grant System Audio Recording permission. If needed, also renew microphone permission for the changed local signature.
2. Choose physical speakers as the macOS sound output and in the calling/playback app. Keep MetalVoice2's output on BlackHole. Details must name those speakers and show a synchronized reference. This release rejects virtual/aggregate playback routes.
3. Compare Echo Off/On with playback only, your voice only, and simultaneous playback and speech. Use moderate playback volume. Listen from the receiving side without feeding BlackHole back to these speakers. Disable duplicate receiving-app processing for an isolated comparison where possible.
4. Observe reference frames/peak, processing time and dropout counters. Copy reports. A silent playback stream is valid; microphone speech alone must not drive the reference meter.
5. Stop/start repeatedly, change default output, unplug/reconnect devices, sleep/wake, and deny/revoke the extra permission. Confirm clear errors, bounded reference recovery and no leaked capture/aggregate devices. These OS/hardware checks are not covered by the offline suite.
6. `scripts/benchmark.sh 60 off --echo` measures live counters after the menu app is quit. It saves no audio. Measure physical end-to-end delay separately; the displayed timestamp interval is not that measurement.


## LocalVQE live comparison (v0.9, pending)

Reopen `build/MetalVoice2.app` to load the rebuilt binary. Use the same microphone/channel and BlackHole route. Start with WebRTC Off and Enhanced LocalVQE. Echo and Room de-reverb must be grey and unchecked, including if they were enabled in the previous mode. Speak at normal and quiet levels, include louder peaks and pauses, and compare room tails with DPDFNet-2/RNNoise and voice-changer's sup2. Keep the receiving app's processing fixed. LocalVQE has 16 kHz speech bandwidth; do not confuse reduced treble with better reverberation removal. It is not Amazon Voice Focus.

Check Details/Copy report: LocalVQE delay 34 ms (40.96 ms with WebRTC), CPU 2 threads, Echo and WPE inactive, no reference-capture device. Total path latency includes transport. Check underruns, overruns, resyncs and compute p99 under normal machine load. Switch away to confirm the old checkbox selections return; switching while running briefly restarts the stream. Stop/start and sleep/wake should clear temporal history. No additional System Audio Recording permission is needed in LocalVQE mode; an ad-hoc rebuild can require renewed microphone permission. Agent qualification was offline and did not record microphone/playback audio.
