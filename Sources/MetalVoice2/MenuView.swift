import SwiftUI
import AppKit
import AudioCore

struct PanelEnableControls: View {
    @Binding var enabled: Bool
    @Binding var atLaunch: Bool
    let feature: String
    let enableHelp: String
    let launchHelp: String
    var launchAvailable = true

    var body: some View {
        Toggle("Enable", isOn: $enabled)
            .toggleStyle(.switch).fixedSize()
            .accessibilityLabel("Enable " + feature).panelHelp(enableHelp)
        Toggle("Enable at launch", isOn: $atLaunch)
            .toggleStyle(.checkbox).fixedSize().disabled(!enabled || !launchAvailable)
            .accessibilityLabel("Enable " + feature + " at launch").panelHelp(launchHelp)
    }
}

struct MenuView: View {
    static let panelWidth: CGFloat = 460

    @ObservedObject var audio: AudioController
    @AppStorage("showVoiceChangerTab") private var showVoiceChanger = false
    var presentPanel: (NSOpenPanel) -> NSApplication.ModalResponse = { $0.runModal() }
    var onContentHeightChange: (CGFloat) -> Void = { _ in }

    var body: some View {
        VStack(spacing: 0) {
            content
            if audio.showDetails {
                diagnostics.padding(.horizontal, 16).padding(.bottom, 16)
            }
        }
        .frame(width: Self.panelWidth)
        .fixedSize(horizontal: false, vertical: true)
        .onGeometryChange(for: CGFloat.self) { ceil($0.size.height) } action: { onContentHeightChange($0) }
        .transaction { $0.animation = nil }
    }

    private var content: some View {
        VStack(alignment: .leading, spacing: 12) {
            if showVoiceChanger {
                VoiceChangerView(audio: audio, library: audio.voices, presentPanel: presentPanel)
            } else {
                microphoneControls
            }
            Divider()
            HStack(alignment: .center, spacing: 8) {
                Text("Audio path latency").font(.subheadline)
                Spacer(minLength: 0)
                Text(audio.running && audio.stats.latency_valid != 0 ? String(format: "%.1f", audio.stats.latency_ms) : "—")
                    .font(.system(size: 30, weight: .medium, design: .rounded)).monospacedDigit()
                Text("ms").font(.caption).foregroundStyle(.secondary)
                VStack(alignment: .trailing, spacing: 2) {
                    Text("p50  \(latency(audio.stats.latency_p50_ms))").panelHelp("Median audio-path delay.")
                    Text("p95  \(latency(audio.stats.latency_p95_ms))").panelHelp("95% of measured delays fall at or below this value.")
                }
                .font(.caption2.monospacedDigit()).foregroundStyle(.secondary)
                .padding(.leading, 8)
            }
            .panelHelp("Time from microphone capture to virtual output, including active processing.")
            VStack(alignment: .trailing, spacing: 4) {
                HStack(alignment: .top, spacing: 12) {
                    TimelineView(.animation(minimumInterval: 1.0 / 24, paused: !audio.running || !audio.panelVisible)) { _ in
                        meters(audio.running ? audio.meterLevels() : MVMeterLevels())
                    }
                    Button {
                        var transaction = Transaction()
                        transaction.disablesAnimations = true
                        withTransaction(transaction) { audio.showDetails.toggle() }
                    } label: {
                        HStack(spacing: 4) {
                            Image(systemName: audio.showDetails ? "chevron.down" : "chevron.right").frame(width: 9)
                            Text("Details")
                        }
                    }
                    .panelHelp(audio.showDetails ? "Hide transport details" : "Show transport details")
                    .accessibilityValue(audio.showDetails ? "Expanded" : "Collapsed")
                }
                Text(audio.status).font(.caption).foregroundStyle(.secondary).lineLimit(1).panelHelp(audio.status)
            }
            if let error = audio.error {
                Text(error).font(.caption).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding([.horizontal, .bottom], 16)
        .frame(width: Self.panelWidth)
    }

    @ViewBuilder private var microphoneControls: some View {
        HStack {
            PanelEnableControls(enabled: Binding(get: { audio.cleanupEnabled }, set: audio.setCleanupEnabled),
                                atLaunch: $audio.enableOnLaunch, feature: "microphone cleanup",
                                enableHelp: "Apply the selected cleanup filters to the microphone.",
                                launchHelp: "Start audio with microphone cleanup when MetalVoice2 opens.")
            Spacer()
        }.disabled(audio.busy)
        microphoneSettings.disabled(!audio.cleanupEnabled || audio.busy)
            .opacity(audio.cleanupEnabled ? 1 : 0.6)
    }

    @ViewBuilder private var microphoneSettings: some View {
        HStack(alignment: .top, spacing: 8) {
            VStack(alignment: .leading, spacing: 4) {
                Text("Microphone input").panelHelp("Physical microphone to capture.").font(.caption).foregroundStyle(.secondary)
                Picker("Microphone input", selection: Binding(get: { audio.inputUID }, set: audio.chooseInput)) {
                    if audio.selectedInput == nil { Text("Select microphone").tag(audio.inputUID) }
                    ForEach(audio.inputs) { Text($0.name).tag($0.uid) }
                }
                .labelsHidden().frame(maxWidth: .infinity, alignment: .leading)
                .panelHelp("Choose the microphone to capture. " + (audio.selectedInput?.name ?? ""))
            }.frame(maxWidth: .infinity)
            VStack(alignment: .leading, spacing: 4) {
                Text("Microphone output").panelHelp("Virtual device your calling app uses as its microphone.").font(.caption).foregroundStyle(.secondary)
                Picker("Microphone output", selection: Binding(get: { audio.outputUID }, set: audio.chooseOutput)) {
                    if audio.selectedOutput == nil { Text("Select output").tag(audio.outputUID) }
                    ForEach(audio.outputs) { Text($0.name).tag($0.uid) }
                }
                .labelsHidden().frame(maxWidth: .infinity, alignment: .leading)
                .panelHelp("Send processed audio to this virtual device. " + (audio.selectedOutput?.name ?? ""))
            }.frame(maxWidth: .infinity)
            VStack(alignment: .leading, spacing: 4) {
                Text("Channels").panelHelp("Choose one input channel from the microphone.").font(.caption).foregroundStyle(.secondary)
                Picker("Input channel", selection: Binding(get: { audio.channel }, set: audio.chooseChannel)) {
                    ForEach(0..<max(1, audio.selectedInput?.inputs ?? 1), id: \.self) { Text("\($0 + 1)").tag($0) }
                }
                .labelsHidden().frame(maxWidth: .infinity)
                .disabled((audio.selectedInput?.inputs ?? 0) <= 1)
                .panelHelp("Select the microphone channel to process.")
            }.frame(width: 56)
        }
        if audio.outputs.isEmpty {
            Text("Select a virtual device such as BlackHole to send audio to your calling app.")
                .font(.caption).foregroundStyle(.secondary)
        }
        Divider()
        HStack(spacing: 20) {
            Toggle("Echo", isOn: Binding(get: { audio.echoCancellation && audio.separateRoomControlsAvailable }, set: audio.setEchoCancellation))
                .toggleStyle(.checkbox)
                .disabled(!audio.separateRoomControlsAvailable || audio.busy || (!audio.echoCancellation && !audio.suppressionSupported))
                .panelHelp(audio.separateRoomControlsAvailable ? "Remove Mac speaker audio picked up by the microphone." : "LocalVQE handles room cleanup and disables separate echo cancellation.")
            Toggle("De-reverb", isOn: Binding(get: { audio.roomDereverb && audio.separateRoomControlsAvailable }, set: audio.setRoomDereverb))
                .toggleStyle(.checkbox)
                .disabled(!audio.separateRoomControlsAvailable || audio.busy || (!audio.roomDereverb && !audio.suppressionSupported))
                .panelHelp(audio.separateRoomControlsAvailable ? "Reduce room reflections that make your voice sound hollow." : "LocalVQE already reduces room reflections.")
            Spacer()
        }
        HStack(spacing: 16) {
            Picker("WebRTC", selection: Binding(get: { audio.webRTCSelection }, set: audio.setWebRTCSelection)) {
                Text("Off").tag(-1)
                Text("Low").tag(0).disabled(!audio.suppressionSupported)
                Text("Medium").tag(1).disabled(!audio.suppressionSupported)
                Text("High").tag(2).disabled(!audio.suppressionSupported)
                Text("Very high").tag(3).disabled(!audio.suppressionSupported)
            }
            .disabled(audio.busy)
            .panelHelp("Reduce steady background noise. Higher strengths suppress more noise.")
            Picker("Enhanced", selection: Binding(get: { audio.enhancedSuppression }, set: audio.setEnhancedSuppression)) {
                ForEach(EnhancedSuppression.allCases, id: \.self) { mode in
                    Text(mode.label).tag(mode).disabled(mode != .off && !audio.suppressionSupported).help(mode.helpText)
                }
            }
            .disabled(audio.busy)
            .panelHelp(audio.enhancedSuppression.helpText)
        }
        if !audio.suppressionSupported {
            Text("Select a microphone running at 48 kHz to use suppression.").font(.caption).foregroundStyle(.secondary)
        }
    }

    private var diagnostics: some View {
        VStack(spacing: 6) {
            Divider()
            HStack {
                Text("Transport details").foregroundStyle(.secondary)
                Spacer()
                Button("Copy report") { audio.copyReport() }.disabled(!audio.running || audio.busy)
                    .buttonStyle(.plain).panelHelp("Copy device, latency and processor diagnostics.")
            }.padding(.bottom, 4)
            if audio.effectiveEchoCancellation {
                detail("Echo engine", "WebRTC AEC3 · 48 kHz")
                    .panelHelp("Algorithm used to cancel speaker playback.")
                detail("Playback reference", audio.running ? nativeString(audio.stats.echo_reference_device) : "macOS default sound output")
                    .panelHelp("Mac output used to identify speaker echo.")
                detail("Reference status", audio.echoReferenceStatus)
                    .panelHelp("Whether playback and microphone audio are synchronized.")
                detail("Echo delay / reference buffer", String(format: "%.2f / %.2f ms", audio.stats.echo_delay_ms, audio.stats.echo_reference_headroom_ms))
                    .panelHelp("Echo processing delay and extra playback-reference buffering.")
                if audio.stats.echo_erle_db >= 0 && audio.running {
                    detail("Echo reduction estimate", String(format: "%.1f dB", audio.stats.echo_erle_db))
                        .panelHelp("Estimated reduction in speaker echo.")
                }
            }
            if audio.activeEnhanced == .deepFilterNet3 {
                detail("DeepFilterNet3 engine", deepFilterEngineLabel(audio.stats))
                    .panelHelp("Compute devices preferred by the active DeepFilterNet3 backend.")
            }
            if audio.activeEnhanced == .dpdfNet2 {
                detail("DPDFNet-2 engine", dpdfEngineLabel(audio.stats))
                    .panelHelp("Compute backend selected for DPDFNet-2.")
            }
            if audio.activeEnhanced == .localVQE {
                detail("LocalVQE", "Joint v1.3 · 16 kHz")
                    .panelHelp("Active voice cleanup model and its speech sample rate.")
                detail("LocalVQE engine", audio.running ? "GGML CPU · \(audio.stats.localvqe_threads) threads" : "GGML CPU · 2 threads")
                    .panelHelp("Backend and worker count for voice cleanup.")
                detail("Playback echo cancellation", "Inactive in this mode")
                    .panelHelp("Whether the active model cancels speaker playback.")
            }
            detail("Rate (in / out)", "\(Int(audio.stats.input_rate)) / \(Int(audio.stats.output_rate)) Hz")
                .panelHelp("Input and output samples processed each second.")
            detail("Buffer (in / out)", "\(audio.stats.input_buffer_frames) / \(audio.stats.output_buffer_frames) frames")
                .panelHelp("Audio frames per device callback.")
            detail("Queue / target", String(format: "%.1f / %.1f ms", audio.stats.queue_ms, audio.stats.target_queue_ms))
                .panelHelp("Current buffered audio and the desired queue depth.")
            detail("Clock correction", String(format: "%+.0f ppm", audio.stats.drift_ppm))
                .panelHelp("Resampling adjustment for drift between the two device clocks.")
            detail("Underruns / overruns", "\(audio.stats.underruns) / \(audio.stats.overruns)")
                .panelHelp("Counts of empty output buffers and dropped input frames.")
            detail("Resynchronizations", "\(audio.stats.resyncs)")
                .panelHelp("Times the audio queue realigned the devices.")
            detail("Maximum latency", latency(audio.stats.latency_max_ms))
                .panelHelp("Longest audio-path delay measured this run.")
            if audio.processingEnabled {
                if audio.effectiveRoomDereverb {
                    detail("Room de-reverb (WPE)", String(format: "%.2f ms", audio.stats.room_dereverb_delay_ms))
                        .panelHelp("Delay added by the room reflection filter.")
                }
                if audio.activeWebRTCLevel >= 0 {
                    detail("WebRTC delay", String(format: "%.2f ms", audio.stats.webrtc_delay_ms))
                        .panelHelp("Delay added by this processor.")
                }
                if audio.activeEnhanced == .rnnoise {
                    detail("RNNoise delay", String(format: "%.2f ms", audio.stats.rnnoise_delay_ms))
                        .panelHelp("Delay added by this processor.")
                }
                if audio.activeEnhanced == .deepFilterNet3 {
                    detail("DeepFilterNet3 delay", String(format: "%.2f ms", audio.stats.deepfilter_delay_ms))
                        .panelHelp("Delay added by this processor.")
                }
                if audio.activeEnhanced == .dpdfNet2 {
                    detail("DPDFNet-2 delay", String(format: "%.2f ms", audio.stats.dpdf_delay_ms))
                        .panelHelp("Delay added by this processor.")
                }
                if audio.activeEnhanced == .localVQE {
                    detail("LocalVQE delay", String(format: "%.2f ms", audio.stats.localvqe_delay_ms))
                        .panelHelp("Delay added by this processor.")
                }
                detail("Total suppression delay", String(format: "%.2f ms", audio.stats.suppression_delay_ms))
                    .panelHelp("Combined delay of the active microphone filters.")
                detail("Suppression work (avg / max)", String(format: "%.3f / %.3f ms", audio.stats.suppression_mean_ms, audio.stats.suppression_max_ms))
                    .panelHelp("Average and longest cleanup time per audio block.")
                detail("Suppression work (p99)", String(format: "%.2f ms", audio.stats.suppression_p99_ms))
                    .panelHelp("99% of cleanup blocks finished within this time.")
            }
        }
        .font(.caption)
        .frame(maxWidth: .infinity, alignment: .topLeading)
    }

    private func latency(_ value: Double) -> String {
        audio.running && audio.stats.latency_samples > 0 ? String(format: "%.1f ms", value) : "—"
    }
    private func detail(_ label: String, _ value: String) -> some View {
        HStack { Text(label).foregroundStyle(.secondary); Spacer(); Text(value).monospacedDigit() }
    }
    private func meters(_ levels: MVMeterLevels) -> some View {
        HStack(alignment: .top, spacing: 18) {
            if audio.hasVoice {
                VoiceInputMeter(library: audio.voices, applied: audio.running ? audio.activeVoicePreset : nil,
                               rms: levels.voice_rms)
                    .frame(maxWidth: .infinity)
            } else {
                meter("Input", peak: levels.input_peak).frame(maxWidth: .infinity)
            }
            meter("Output", peak: levels.output_peak).frame(maxWidth: .infinity)
        }
    }
    private func meter(_ label: String, peak: Double) -> some View {
        HStack(spacing: 6) {
            Text(label).font(.caption2).foregroundStyle(.secondary)
            ProgressView(value: audio.running ? min(1, max(0, (20 * log10(max(peak, 0.001)) + 60) / 60)) : 0)
                .tint(peak >= 1 ? .orange : .accentColor)
        }.panelHelp(label == "Input" ? "Peak level from the selected microphone channel." : "Peak level sent to the virtual microphone.")
    }
}

private extension EnhancedSuppression {
    var helpText: String {
        switch self {
        case .off: return "Choose a neural model for additional microphone cleanup."
        case .rnnoise: return "RNNoise reduces background noise with a lightweight speech model."
        case .deepFilterNet3: return "DeepFilterNet3 reduces noise across the full speech frequency range."
        case .dpdfNet2: return "DPDFNet-2 separates speech from background noise."
        case .localVQE: return "LocalVQE reduces background noise and room reflections together."
        }
    }
}

private struct VoiceInputMeter: View {
    @ObservedObject var library: VoiceLibrary
    let applied: VoicePreset?
    let rms: Double

    // Levels share the gate slider's −120…0 dBFS range. The cutoff uses a separate log Hz scale.
    private func levelPosition(_ db: Double) -> Double { min(1, max(0, (db + 120) / 120)) }
    private func frequencyPosition(_ hz: Double) -> Double { min(1, max(0, log(hz / 20) / log(1000))) }
    private var rmsDB: Double { 20 * log10(max(0.000001, rms)) }
    private func gateLabel(_ value: Double, unit: String) -> String { value <= -120 ? "Gate off" : "Gate \(Int(value)) \(unit)" }
    private func cutoffLabel(_ value: Double) -> String { value <= 20 ? "High-pass off" : "High-pass \(Int(value)) Hz" }

    var body: some View {
        if let draft = library.editedPreset, let saved = library.selected?.preset {
            let current = applied ?? saved
            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 4) {
                    Text("Input").panelHelp("Microphone level after cleanup, measured in dB relative to digital full scale (dBFS).")
                    Text(rms > 0 ? "\(Int(rmsDB.rounded())) dB" : "—")
                    Spacer(minLength: 2)
                    Text(gateLabel(current.gate, unit: "dB")).panelHelp("Applied voice conversion threshold, measured in dBFS.")
                }
                .lineLimit(1)
                rail(fill: levelPosition(rmsDB), marker: current.gate > -120 ? levelPosition(current.gate) : nil,
                     pending: draft.gate != current.gate && draft.gate > -120 ? levelPosition(draft.gate) : nil)
                .panelHelp("Short-term RMS after cleanup. The gate marker shows the applied threshold. Gate hold keeps quiet speech audible after a crossing. Scale: −120 to 0 dBFS.")
                HStack(spacing: 6) {
                    Text(cutoffLabel(current.highPass)).fixedSize()
                    rail(fill: frequencyPosition(current.highPass), marker: current.highPass > 20 ? frequencyPosition(current.highPass) : nil,
                         pending: draft.highPass != current.highPass ? frequencyPosition(draft.highPass) : nil, fillColor: .secondary.opacity(0.45))
                }
                .panelHelp("High-pass cutoff on the converted voice. Frequency increases from 20 Hz to 20 kHz. A red marker previews an edit.")
            }
            .font(.caption2.monospacedDigit()).foregroundStyle(.secondary)
            .accessibilityElement(children: .ignore)
            .accessibilityLabel("Voice input")
            .accessibilityValue("\(Int(rmsDB.rounded())) dBFS RMS, \(gateLabel(current.gate, unit: "dBFS")), \(cutoffLabel(current.highPass))")
        }
    }

    private func rail(fill: Double, marker: Double?, pending: Double?, fillColor: Color = .accentColor) -> some View {
        GeometryReader { geometry in
            let width = geometry.size.width
            ZStack(alignment: .leading) {
                Capsule().fill(.quaternary).frame(height: 5)
                Capsule().fill(fillColor).frame(width: width * fill, height: 5)
                if let marker {
                    Rectangle().fill(.primary).frame(width: 2, height: 9)
                        .offset(x: (width - 2) * marker)
                }
                if let pending {
                    Rectangle().fill(.red.opacity(0.65)).frame(width: 2, height: 9)
                        .offset(x: (width - 2) * pending)
                }
            }.frame(height: 9)
        }.frame(height: 9)
    }
}

/// This toolbar has its own host, outside the document that resizes or scrolls.
struct MenuHeader: View {
    @ObservedObject var audio: AudioController
    @Environment(\.colorScheme) private var colorScheme
    @AppStorage("showVoiceChangerTab") private var showVoiceChanger = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 8) {
                if let logo = NSImage(named: "MetalVoice2-logo") {
                    let artwork = Image(nsImage: logo).resizable().scaledToFit()
                    artwork
                        .overlay {
                            if colorScheme == .dark {
                                // The wordmark starts one third of the way across the artwork.
                                GeometryReader { bounds in
                                    Color.white.padding(.leading, bounds.size.width * 0.327)
                                }
                                .mask(artwork)
                            }
                        }
                        .frame(width: 122, height: 29)
                        .accessibilityLabel("MetalVoice2").panelHelp("MetalVoice2 microphone processor.")
                } else {
                    Text("MetalVoice2").font(.headline)
                }
                Text("Microphone noise reduction").panelHelp("Clean up microphone audio before sending it to other apps.")
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: true, vertical: false)
                Spacer(minLength: 0)
                Button("Refresh devices") { audio.refreshDevices() }
                    .disabled(audio.running || audio.busy).panelHelp("Scan for connected audio devices.")
                Button("Quit") { NSApplication.shared.terminate(nil) }.panelHelp("Stop audio and quit MetalVoice2.")
                Circle().fill(audio.running ? (audio.muted ? Color.orange : Color.green) : Color.gray).frame(width: 8, height: 8).panelHelp(audio.running ? (audio.muted ? "Audio is muted." : "Audio is running.") : "Audio is stopped.")
            }
            .buttonStyle(.plain).font(.caption)
            HStack {
                PanelTabs(voice: $showVoiceChanger)
                    .frame(width: 216, height: 24)
                Spacer()
                Button { audio.toggleMute() } label: {
                    HStack(spacing: 4) {
                        Image(systemName: audio.muted ? "speaker.slash.fill" : "speaker.wave.2")
                        Text(audio.muted ? "Unmute" : "Mute")
                    }
                    .frame(width: 76, height: 24)
                    .foregroundStyle(audio.muted ? Color.white : .primary)
                    .background(audio.muted ? Color.blue : Color.secondary.opacity(0.14),
                                in: RoundedRectangle(cornerRadius: 6))
                }
                .buttonStyle(.plain)
                .disabled(!audio.running || audio.busy)
                .accessibilityValue(audio.muted ? "Muted" : "Sound on")
                .panelHelp("Silence or restore the virtual microphone output for both tabs.")
            }
        }
        .padding([.top, .horizontal], 16)
        .padding(.bottom, 12)
        .frame(width: MenuView.panelWidth)
        .fixedSize()
    }
}

private struct PanelTabs: NSViewRepresentable {
    @Binding var voice: Bool

    func makeNSView(context: Context) -> NSSegmentedControl {
        let control = NSSegmentedControl(labels: ["Microphone", "Voice Changer"], trackingMode: .selectOne,
                                         target: context.coordinator, action: #selector(Coordinator.choose(_:)))
        control.segmentStyle = .rounded
        control.segmentDistribution = .fillEqually
        control.selectedSegmentBezelColor = .systemBlue
        for i in 0..<2 { control.setWidth(108, forSegment: i) }
        control.setToolTip("Microphone input, output and cleanup settings.", forSegment: 0)
        control.setToolTip("Voice models and conversion settings.", forSegment: 1)
        return control
    }

    func updateNSView(_ control: NSSegmentedControl, context: Context) {
        let selection = voice ? 1 : 0
        if control.selectedSegment != selection { control.selectedSegment = selection }
    }

    func makeCoordinator() -> Coordinator { Coordinator(selection: $voice) }
    final class Coordinator: NSObject {
        let selection: Binding<Bool>
        init(selection: Binding<Bool>) { self.selection = selection }
        @objc func choose(_ sender: NSSegmentedControl) { selection.wrappedValue = sender.selectedSegment == 1 }
    }
}
