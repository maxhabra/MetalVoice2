import SwiftUI
import AppKit

struct MenuView: View {
    static let panelWidth: CGFloat = 460
    // Initial size only. Actual height follows content and grows downward for Details.
    static let panelHeight: CGFloat = 324

    @ObservedObject var audio: AudioController
    let viewportHeight: CGFloat

    private var naturalHeight: CGFloat { audio.panelContentHeight }
    var onHeightChange: (CGFloat) -> Void = { _ in }

    var body: some View {
        ScrollView(.vertical, showsIndicators: false) {
            VStack(spacing: 0) {
                content.fixedSize(horizontal: false, vertical: true)
                if audio.showDetails {
                    diagnostics.padding(.horizontal, 16).padding(.bottom, 16)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            .frame(width: Self.panelWidth)
            .background {
                GeometryReader { geometry in
                    Color.clear.preference(key: PanelHeightPreference.self, value: geometry.size.height)
                }
            }
        }
        .scrollDisabled(naturalHeight <= viewportHeight)
        .frame(width: Self.panelWidth, height: min(naturalHeight, viewportHeight), alignment: .top)
        .transaction { $0.animation = nil }
        .onPreferenceChange(PanelHeightPreference.self) { measured in
            guard measured > 0 else { return }
            let height = ceil(measured)
            if audio.panelContentHeight != height { audio.panelContentHeight = height }
            // Resize AppKit after SwiftUI has finished the measurement pass.
            // Resizing inside that pass can be overwritten by its old frame.
            DispatchQueue.main.async { onHeightChange(min(audio.panelContentHeight, viewportHeight)) }
        }
        .onAppear { audio.panelVisible = true; audio.refreshDevices() }
        .onDisappear { audio.panelVisible = false }
    }

    private var content: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Image(systemName: "waveform").font(.title2).foregroundStyle(.tint)
                VStack(alignment: .leading, spacing: 3) {
                    Text("MetalVoice2").font(.headline)
                    Text("Microphone noise reduction").font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                Button("Refresh devices") { audio.refreshDevices() }
                    .disabled(audio.running || audio.busy)
                Button("Quit") { NSApplication.shared.terminate(nil) }
                Circle().fill(audio.running ? (audio.muted ? Color.orange : Color.green) : Color.gray).frame(width: 8, height: 8)
            }
            .buttonStyle(.plain).font(.caption)
            HStack(alignment: .top, spacing: 8) {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Microphone").font(.caption).foregroundStyle(.secondary)
                    Picker("Microphone", selection: Binding(get: { audio.inputUID }, set: audio.chooseInput)) {
                        if audio.selectedInput == nil { Text("Select microphone").tag(audio.inputUID) }
                        ForEach(audio.inputs) { Text($0.name).tag($0.uid) }
                    }
                    .labelsHidden().frame(maxWidth: .infinity, alignment: .leading)
                    .disabled(audio.running || audio.busy)
                    .help(audio.selectedInput?.name ?? "Choose your microphone")
                }.frame(maxWidth: .infinity)
                VStack(alignment: .leading, spacing: 4) {
                    Text("Virtual microphone").font(.caption).foregroundStyle(.secondary)
                    Picker("Virtual microphone", selection: Binding(get: { audio.outputUID }, set: audio.chooseOutput)) {
                        if audio.selectedOutput == nil { Text("Select output").tag(audio.outputUID) }
                        ForEach(audio.outputs) { Text($0.name).tag($0.uid) }
                    }
                    .labelsHidden().frame(maxWidth: .infinity, alignment: .leading)
                    .disabled(audio.running || audio.busy)
                    .help(audio.selectedOutput?.name ?? "Choose a virtual output such as BlackHole")
                }.frame(maxWidth: .infinity)
                VStack(alignment: .leading, spacing: 4) {
                    Text("Channel").font(.caption).foregroundStyle(.secondary)
                    Picker("Input channel", selection: $audio.channel) {
                        ForEach(0..<max(1, audio.selectedInput?.inputs ?? 1), id: \.self) { Text("\($0 + 1)").tag($0) }
                    }
                    .labelsHidden().frame(maxWidth: .infinity)
                    .disabled(audio.running || audio.busy || (audio.selectedInput?.inputs ?? 0) <= 1)
                    .onChange(of: audio.channel) { _, _ in audio.saveSelection() }
                }.frame(width: 56)
            }
            if audio.outputs.isEmpty {
                Text("Select a virtual device such as BlackHole to send audio to your calling app.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Divider()
            HStack(spacing: 20) {
                Toggle("Echo", isOn: Binding(get: { audio.effectiveEchoCancellation }, set: audio.setEchoCancellation))
                    .toggleStyle(.checkbox)
                    .disabled(!audio.separateRoomControlsAvailable || audio.busy || (!audio.echoCancellation && !audio.suppressionSupported))
                    .help(!audio.separateRoomControlsAvailable ? "Inactive while LocalVQE is selected. Your Echo setting is restored when you switch models." : "Cancel Mac speaker playback picked up by your microphone with WebRTC AEC3. Uses the macOS default sound output; your calling app must play there. Requires System Audio Recording permission. Changes briefly restart audio.")
                Toggle("Room de-reverb", isOn: Binding(get: { audio.effectiveRoomDereverb }, set: audio.setRoomDereverb))
                    .toggleStyle(.checkbox)
                    .disabled(!audio.separateRoomControlsAvailable || audio.busy || (!audio.roomDereverb && !audio.suppressionSupported))
                    .help(!audio.separateRoomControlsAvailable ? "LocalVQE already performs room cleanup. Your WPE setting is restored when you switch models." : "Reduce room reverberation with conservative online WPE (experimental). Adds 10 ms. May affect voice tone. Changes briefly restart audio.")
                Spacer()
            }
            .controlSize(.small).font(.subheadline)
            HStack(spacing: 16) {
                Picker("WebRTC", selection: Binding(get: { audio.webRTCSelection }, set: audio.setWebRTCSelection)) {
                    Text("Off").tag(-1)
                    Text("Low").tag(0).disabled(!audio.suppressionSupported)
                    Text("Medium").tag(1).disabled(!audio.suppressionSupported)
                    Text("High").tag(2).disabled(!audio.suppressionSupported)
                    Text("Very high").tag(3).disabled(!audio.suppressionSupported)
                }
                .disabled(audio.busy)
                .help("WebRTC noise suppression. Changes briefly restart the audio stream.")
                Picker("Enhanced", selection: Binding(get: { audio.enhancedSuppression }, set: audio.setEnhancedSuppression)) {
                    ForEach(EnhancedSuppression.allCases, id: \.self) { mode in
                        Text(mode.label).tag(mode).disabled(mode != .off && !audio.suppressionSupported)
                    }
                }
                .disabled(audio.busy)
                .help(audio.enhancedSuppression == .localVQE ? "LocalVQE v1.3: noise and room cleanup at 16 kHz speech bandwidth. Separate Echo and WPE are inactive. No playback capture or noise gate. Adds 34 ms; uses two CPU threads." : "Enhanced noise suppression. Use alone or together with WebRTC.")
            }
            .controlSize(.small)
            .font(.subheadline)
            if !audio.suppressionSupported {
                Text("Select a microphone running at 48 kHz to use suppression.").font(.caption).foregroundStyle(.secondary)
            }
            Divider()
            HStack(alignment: .center, spacing: 8) {
                Text("Audio path latency").font(.subheadline)
                Spacer(minLength: 0)
                Text(audio.running && audio.stats.latency_valid != 0 ? String(format: "%.1f", audio.stats.latency_ms) : "—")
                    .font(.system(size: 30, weight: .medium, design: .rounded)).monospacedDigit()
                Text("ms").font(.caption).foregroundStyle(.secondary)
                VStack(alignment: .trailing, spacing: 2) {
                    Text("p50  \(latency(audio.stats.latency_p50_ms))")
                    Text("p95  \(latency(audio.stats.latency_p95_ms))")
                }
                .font(.caption2.monospacedDigit()).foregroundStyle(.secondary)
                .padding(.leading, 8)
            }
            .help("Core Audio capture-to-output timestamps, including suppression delay. Not physical or receiving-app latency.")
            HStack(spacing: 18) {
                meter("Input", peak: audio.stats.input_peak)
                meter("Output", peak: audio.stats.output_peak)
            }
            HStack {
                Button(audio.wantedRunning ? "Stop" : "Start") { audio.wantedRunning ? audio.stop() : audio.start() }
                    .buttonStyle(.borderedProminent).disabled(!audio.wantedRunning && !audio.canStart)
                Button(audio.muted ? "Unmute" : "Mute") { audio.toggleMute() }.disabled(!audio.running || audio.busy)
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
                .help(audio.showDetails ? "Hide transport details" : "Show transport details")
                .accessibilityValue(audio.showDetails ? "Expanded" : "Collapsed")
                Spacer()
                Text(audio.status).font(.caption).foregroundStyle(.secondary).lineLimit(1).help(audio.status)
            }
            if let error = audio.error {
                Text(error).font(.caption).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(16)
        .frame(width: Self.panelWidth)
    }

    private var diagnostics: some View {
        VStack(spacing: 6) {
            Divider()
            HStack {
                Text("Transport details").foregroundStyle(.secondary)
                Spacer()
                Button("Copy report") { audio.copyReport() }.disabled(!audio.running || audio.busy)
                    .buttonStyle(.plain)
            }.padding(.bottom, 4)
            if audio.effectiveEchoCancellation {
                detail("Echo engine", "WebRTC AEC3 · 48 kHz")
                detail("Playback reference", audio.running ? nativeString(audio.stats.echo_reference_device) : "macOS default sound output")
                detail("Reference status", audio.echoReferenceStatus)
                detail("Echo delay / reference buffer", String(format: "%.2f / %.2f ms", audio.stats.echo_delay_ms, audio.stats.echo_reference_headroom_ms))
                if audio.stats.echo_erle_db >= 0 && audio.running {
                    detail("Echo reduction estimate", String(format: "%.1f dB", audio.stats.echo_erle_db))
                        .help("AEC3's internal linear echo reduction estimate; not a measured end-to-end quality score.")
                }
            }
            if audio.enhancedSuppression == .deepFilterNet3 {
                detail("DeepFilterNet3 engine", deepFilterEngineLabel(audio.stats))
                    .help("Devices preferred by Apple's compute plan. Core ML may use CPU, GPU, or Neural Engine; this is not hardware utilization telemetry.")
            }
            if audio.enhancedSuppression == .dpdfNet2 {
                detail("DPDFNet-2 engine", dpdfEngineLabel(audio.stats))
                    .help("48 kHz HR model. Uses the CPU configuration qualified for low frame-processing time; Accelerate handles FFT processing.")
            }
            if audio.enhancedSuppression == .localVQE {
                detail("LocalVQE", "Joint v1.3 · 16 kHz")
                detail("LocalVQE engine", audio.running ? "GGML CPU · \(audio.stats.localvqe_threads) threads" : "GGML CPU · 2 threads")
                detail("Playback echo cancellation", "Inactive in this mode")
            }
            detail("Rate (in / out)", "\(Int(audio.stats.input_rate)) / \(Int(audio.stats.output_rate)) Hz")
            detail("Buffer (in / out)", "\(audio.stats.input_buffer_frames) / \(audio.stats.output_buffer_frames) frames")
            detail("Queue / target", String(format: "%.1f / %.1f ms", audio.stats.queue_ms, audio.stats.target_queue_ms))
            detail("Clock correction", String(format: "%+.0f ppm", audio.stats.drift_ppm))
            detail("Underruns / overruns", "\(audio.stats.underruns) / \(audio.stats.overruns)")
            detail("Resynchronizations", "\(audio.stats.resyncs)")
            detail("Maximum latency", latency(audio.stats.latency_max_ms))
            if audio.processingEnabled {
                if audio.effectiveRoomDereverb {
                    detail("Room de-reverb (WPE)", String(format: "%.2f ms", audio.stats.room_dereverb_delay_ms))
                }
                if audio.noiseSuppression {
                    detail("WebRTC delay", String(format: "%.2f ms", audio.stats.webrtc_delay_ms))
                }
                if audio.enhancedSuppression == .rnnoise {
                    detail("RNNoise delay", String(format: "%.2f ms", audio.stats.rnnoise_delay_ms))
                }
                if audio.enhancedSuppression == .deepFilterNet3 {
                    detail("DeepFilterNet3 delay", String(format: "%.2f ms", audio.stats.deepfilter_delay_ms))
                }
                if audio.enhancedSuppression == .dpdfNet2 {
                    detail("DPDFNet-2 delay", String(format: "%.2f ms", audio.stats.dpdf_delay_ms))
                }
                if audio.enhancedSuppression == .localVQE {
                    detail("LocalVQE delay", String(format: "%.2f ms", audio.stats.localvqe_delay_ms))
                }
                detail("Total suppression delay", String(format: "%.2f ms", audio.stats.suppression_delay_ms))
                detail("Suppression work (avg / max)", String(format: "%.3f / %.3f ms", audio.stats.suppression_mean_ms, audio.stats.suppression_max_ms))
                detail("Suppression work (p99)", String(format: "%.2f ms", audio.stats.suppression_p99_ms))
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
    private func meter(_ label: String, peak: Double) -> some View {
        HStack(spacing: 6) {
            Text(label).font(.caption2).foregroundStyle(.secondary)
            ProgressView(value: audio.running ? min(1, max(0, (20 * log10(max(peak, 0.001)) + 60) / 60)) : 0)
                .tint(peak >= 1 ? .orange : .accentColor)
        }
    }
}

private struct PanelHeightPreference: PreferenceKey {
    static let defaultValue: CGFloat = 0
    static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) { value = max(value, nextValue()) }
}
