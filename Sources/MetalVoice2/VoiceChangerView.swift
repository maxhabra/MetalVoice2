import SwiftUI
import AppKit
import UniformTypeIdentifiers

// Use the property wrapper available in the macOS 15 Command Line Tools SDK.
private typealias ViewState<Value> = SwiftUI.State<Value>

struct VoiceChangerView: View {
    @ObservedObject var audio: AudioController
    @ObservedObject var library: VoiceLibrary
    @AppStorage("rvcAssetsFolder") private var assetFolder = ""
    @ViewState private var fileError: String?
    var presentPanel: (NSOpenPanel) -> NSApplication.ModalResponse

    private var preset: VoicePreset { library.editedPreset ?? VoicePreset() }

    private var applied: VoicePreset? { library.selected?.preset }

    private func changed<T: Equatable>(_ key: KeyPath<VoicePreset, T>) -> Bool {
        applied.map { $0[keyPath: key] != preset[keyPath: key] } ?? false
    }

    private func value<T>(_ key: WritableKeyPath<VoicePreset, T>) -> Binding<T> {
        Binding(get: { preset[keyPath: key] }, set: { newValue in
            var updated = preset
            updated[keyPath: key] = newValue
            library.setDraft(updated)
        })
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                PanelEnableControls(enabled: Binding(get: { audio.voiceEnabled }, set: audio.setVoiceEnabled),
                                    atLaunch: $audio.voiceOnLaunch, feature: "voice conversion",
                                    enableHelp: "Convert microphone audio using the selected voice.",
                                    launchHelp: "Start audio with the selected voice when MetalVoice2 opens.",
                                    launchAvailable: library.selected != nil || audio.voiceOnLaunch)
                Spacer()
                Button("Model assets…") {
                    let panel = NSOpenPanel()
                    panel.canChooseDirectories = true
                    panel.canChooseFiles = false
                    panel.message = "Select VCClient’s modules folder containing ContentVec and, for RMVPE, its pitch model."
                    if presentPanel(panel) == .OK, let folder = panel.url {
                        assetFolder = folder.path
                        audio.reloadVoice()
                    }
                }
                .disabled(!audio.voiceEnabled)
                .panelHelp("Locate VCClient’s shared ContentVec speech encoder and RMVPE pitch estimator.")
            }
            voiceSettings.disabled(!audio.voiceEnabled)
                .opacity(audio.voiceEnabled ? 1 : 0.6)
            if audio.voiceEnabled && library.selected == nil {
                Text("Select or import a voice to start conversion.").font(.caption).foregroundStyle(.secondary)
            }
        }.id(library.catalog.selection).disabled(audio.busy)
    }

    @ViewBuilder private var voiceSettings: some View {
        HStack {
            Picker("Voice", selection: Binding(get: { library.catalog.selection }, set: { selection in
                if selection == nil { audio.setVoiceEnabled(false) }
                library.select(selection)
                if selection != nil { audio.reloadVoice() }
            })) {
                Text("None").tag(nil as URL?)
                ForEach(library.catalog.voices) { voice in
                    Text(voice.name).tag(Optional(voice.id)).help(voice.model.path)
                }
            }
            .panelHelp("Select the voice model used for conversion.")
            Button("Import .pth…") { choose(index: false) }
                .panelHelp("Add an RVC voice model to your library.")
            Button("Remove") {
                audio.setVoiceEnabled(false)
                library.removeSelected()
            }
            .panelHelp("Remove the selected voice from the library. Its files stay on disk.")
            .disabled(library.selected == nil)
        }
        HStack {
            Text("Index").panelHelp("Optional file of voice features matched to this model.")
            Text(library.selected?.index?.lastPathComponent ?? "None")
                .foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                .panelHelp(library.selected?.index?.path ?? "Optional retrieval index")
            Spacer()
            if library.selected?.index != nil {
                Button("Detach") { try? library.attach(nil); audio.reloadVoice() }
                    .panelHelp("Unlink the index from this voice.")
            }
            Button(library.selected?.index == nil ? "Attach .index…" : "Replace…") { choose(index: true) }
                .panelHelp("Choose the retrieval index trained for this voice.")
                .disabled(library.selected == nil)
        }
        if let fileError { Text(fileError).font(.caption).foregroundStyle(.orange) }
        Grid(alignment: .leading, horizontalSpacing: 10, verticalSpacing: 7) {
            row("Pitch", \.pitch, -24...24, 1, "st", help: "Raise or lower the converted voice in semitones.")
            row("Index blend", \.indexStrength, 0...100, 1, "%", help: "Blend in features from the attached voice index.")
                .disabled(library.selected?.index == nil)
            row("Chunk", \.chunk, 50...1000, 10, "ms", help: "New audio collected per conversion. Smaller chunks reduce waiting time.")
            row("Extra history", \.extra, 0...1500, 10, "ms", help: "Earlier audio the model uses as context for each chunk.")
            row("Noise gate", \.gate, -120...0, 1, "dBFS", off: -120, help: "Set how quiet the input can be before conversion goes silent.")
            row("Gate hold", \.gateHold, 0...2000, 1, "ms", help: "Keep converting quiet syllables after the input falls below the gate.")
        }
        .disabled(library.selected == nil)
        CutoffRange(low: value(\.highPass), high: value(\.lowPass),
                    lowChanged: changed(\.highPass), highChanged: changed(\.lowPass)).disabled(library.selected == nil)
        HStack {
            Text("F0 estimator").panelHelp("Tracks your input pitch before the semitone shift. Choose a neural or signal-based detector.")
            Picker("F0 estimator", selection: value(\.f0)) {
                Text("RMVPE (ONNX)").tag(F0Estimator.rmvpe)
                Text("Harvest").tag(F0Estimator.harvest)
            }
            .labelsHidden().fixedSize()
            .modifier(PendingValue(changed: changed(\.f0)))
            .disabled(library.selected == nil)
            .panelHelp("RMVPE uses a neural pitch model. Harvest estimates pitch from harmonics in the microphone signal.")
            Spacer()
            Button("Cancel") {
                NSApp.keyWindow?.makeFirstResponder(nil)
                library.cancelDraft()
            }.disabled(!library.hasEdits).panelHelp("Discard edits and restore the saved values for this voice.")
            ApplyVoiceButton(pending: library.hasEdits) {
                NSApp.keyWindow?.makeFirstResponder(nil)
                library.applyDraft()
                audio.reloadVoice()
            }
        }
    }

    private func row(_ label: String, _ key: WritableKeyPath<VoicePreset, Double>,
                     _ range: ClosedRange<Double>, _ step: Double, _ unit: String, off: Double? = nil, help: String) -> some View {
        GridRow {
            Text(label).panelHelp(help)
            Group {
                if key == \.gateHold {
                    // Two thousand native tick marks obscure the track. Keep 1 ms precision.
                    Slider(value: Binding(get: { preset.gateHold }, set: { value(key).wrappedValue = $0.rounded() }),
                           in: range) { Text(label) }
                } else {
                    Slider(value: value(key), in: range, step: step) { Text(label) }
                }
            }.labelsHidden().panelHelp(help).tint(changed(key) ? .red.opacity(0.65) : .accentColor)
            NumberEntry(label: label, value: value(key), range: range, step: step, unit: unit, off: off)
                .modifier(PendingValue(changed: changed(key))).panelHelp(help)
        }
    }

    private func choose(index: Bool) {
        let panel = NSOpenPanel()
        let ext = index ? "index" : "pth"
        panel.allowedContentTypes = [UTType(filenameExtension: ext) ?? .data]
        panel.allowsMultipleSelection = !index
        panel.title = index ? "Attach retrieval index" : "Import RVC voices"
        panel.message = "Files stay in their current location."
        guard presentPanel(panel) == .OK else { return }
        do {
            if index, let url = panel.url { try library.attach(url) }
            else { try library.add(panel.urls) }
            audio.reloadVoice()
            fileError = nil
        } catch { fileError = error.localizedDescription }
    }
}

// Logarithmic spacing keeps both rumble and treble cutoffs easy to adjust.
private struct CutoffRange: View {
    @Binding var low: Double
    @Binding var high: Double
    let lowChanged: Bool
    let highChanged: Bool
    @ViewState private var draggingLow: Bool?
    @Environment(\.isEnabled) private var enabled
    private let lowHelp = "Reduce frequencies below this cutoff in the converted voice."
    private let highHelp = "Reduce frequencies above this cutoff in the converted voice."
    private let limits = 20.0...20_000.0
    private func fraction(_ hz: Double) -> Double { log(hz / 20) / log(1000) }

    var body: some View {
        VStack(spacing: 2) {
            HStack {
                Text("High-pass").panelHelp(lowHelp)
                NumberEntry(label: "High-pass", value: $low, range: 20...(high - 10), step: 10, unit: "Hz", off: 20)
                    .modifier(PendingValue(changed: lowChanged)).panelHelp(lowHelp)
                Spacer()
                Text("Low-pass").panelHelp(highHelp)
                NumberEntry(label: "Low-pass", value: $high, range: (low + 10)...20_000, step: 10, unit: "Hz", off: 20_000)
                    .modifier(PendingValue(changed: highChanged)).panelHelp(highHelp)
            }.font(.caption).monospacedDigit()
            GeometryReader { bounds in
                let width = max(1, bounds.size.width - 16)
                let left = 8 + width * fraction(low)
                let right = 8 + width * fraction(high)
                ZStack(alignment: .leading) {
                    Capsule().fill(.quaternary).frame(height: 4).padding(.horizontal, 8)
                    Capsule().fill(Color.accentColor).frame(width: max(0, right-left), height: 4).offset(x: left)
                    thumb(changed: lowChanged).offset(x: left-7)
                    thumb(changed: highChanged).offset(x: right-7)
                }
                .frame(height: 22)
                .contentShape(Rectangle())
                .gesture(DragGesture(minimumDistance: 0).onChanged { drag in
                    let start = drag.startLocation.x
                    let moveLow = draggingLow ?? (abs(start-left) <= abs(start-right))
                    draggingLow = moveLow
                    let t = min(1, max(0, (drag.location.x - 8) / width))
                    let hz = (20 * pow(1000, t) / 10).rounded() * 10
                    if moveLow { low = min(high-10, hz) } else { high = max(low+10, hz) }
                }.onEnded { _ in draggingLow = nil })
            }.frame(height: 22)
            .panelHelp("Drag the left handle for high-pass or the right handle for low-pass.")
            .accessibilityRepresentation {
                VStack {
                    Slider(value: Binding(get: { low }, set: { low = min(high - 10, $0) }), in: limits, step: 10) { Text("High-pass cutoff in hertz") }
                    Slider(value: Binding(get: { high }, set: { high = max(low + 10, $0) }), in: limits, step: 10) { Text("Low-pass cutoff in hertz") }
                }
            }
        }.opacity(enabled ? 1 : 0.5)
    }

    private func thumb(changed: Bool) -> some View {
        Circle().fill(Color(nsColor: .controlBackgroundColor))
            .overlay(Circle().stroke(changed ? Color.red.opacity(0.7) : .secondary, lineWidth: changed ? 2 : 1)).frame(width: 14, height: 14)
    }
}

private struct NumberEntry: View {
    let label: String
    @Binding var value: Double
    let range: ClosedRange<Double>
    let step: Double
    let unit: String
    var off: Double?
    @ViewState private var entry: String?
    @FocusState private var focused: Bool

    private var formatted: String { value == off ? "Off" : String(Int(value)) }

    private func parsed(_ text: String) -> Double? {
        let text = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if text.lowercased() == "off" { return off }
        guard let n = Int(text.replacingOccurrences(of: "−", with: "-")) else { return nil }
        return min(range.upperBound, max(range.lowerBound, (Double(n) / step).rounded() * step))
    }

    var body: some View {
        HStack(spacing: 3) {
            TextField(label, text: Binding(get: { entry ?? formatted }, set: { text in
                entry = text
                if let number = parsed(text) { value = number }
            }))
            .labelsHidden().textFieldStyle(.roundedBorder).controlSize(.small)
            .multilineTextAlignment(.trailing).monospacedDigit().frame(width: 52)
            .focused($focused)
            .onSubmit { focused = false; entry = nil }
            .onChange(of: focused) { _, active in if !active { entry = nil } }
            .onChange(of: value) { _, newValue in
                if let text = entry, parsed(text) != newValue { entry = nil }
            }
            .accessibilityLabel(label + " in " + unit)
            Text(unit).font(.caption).foregroundStyle(.secondary)
                .frame(width: 27, alignment: .leading)
        }
    }
}

private struct PendingValue: ViewModifier {
    let changed: Bool

    func body(content: Content) -> some View {
        content.padding(.horizontal, 4).padding(.vertical, 1)
            .background(.red.opacity(changed ? 0.20 : 0), in: RoundedRectangle(cornerRadius: 4))
            .accessibilityHint(changed ? "Change awaiting Apply settings" : "")
    }
}

private struct ApplyVoiceButton: View {
    let pending: Bool
    var apply: () -> Void
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        Button("Apply settings", action: apply)
            .disabled(!pending)
            .overlay {
                if pending {
                    let outline = RoundedRectangle(cornerRadius: 5).stroke(.red, lineWidth: 1.5).allowsHitTesting(false)
                    if reduceMotion {
                        outline.opacity(0.7)
                    } else {
                        outline.phaseAnimator([false, true]) { view, bright in
                            view.opacity(bright ? 0.85 : 0.22)
                        } animation: { _ in .easeInOut(duration: 1.2) }
                    }
                }
            }
            .panelHelp("Save the edited settings and use them for this voice.")
            .accessibilityHint(pending ? "Apply pending voice settings" : "")
    }
}
