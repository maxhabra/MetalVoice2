import AppKit
import AVFoundation
import AudioCore

let appVersion = Bundle.main.object(forInfoDictionaryKey: "MVReleaseVersion") as? String
    ?? Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "development"

enum EnhancedSuppression: String, CaseIterable {
    case off, rnnoise, deepFilterNet3 = "deepfilternet3", dpdfNet2 = "dpdfnet2", localVQE = "localvqe"
    var label: String {
        switch self { case .off: "Off"; case .rnnoise: "RNNoise"; case .deepFilterNet3: "DeepFilterNet3"; case .dpdfNet2: "DPDFNet-2"; case .localVQE: "LocalVQE" }
    }
    var nativeMode: Int32 {
        switch self { case .off: 0; case .rnnoise: 1; case .deepFilterNet3: 2; case .dpdfNet2: 3; case .localVQE: 4 }
    }
    static func fromNative(_ mode: Int32) -> Self {
        allCases.first { $0.nativeMode == mode } ?? .off
    }
}

func suppressionModeName(webRTCLevel: Int32, enhanced: Int32, roomDereverb: Bool = false, echo: Bool = false) -> String {
    if echo && enhanced != 4 {
        let rest = suppressionModeName(webRTCLevel: webRTCLevel, enhanced: enhanced, roomDereverb: roomDereverb)
        return rest == "native-pass-through" ? "aec3" : "aec3-and-\(rest)"
    }
    if roomDereverb && enhanced != 4 {
        let rest = suppressionModeName(webRTCLevel: webRTCLevel, enhanced: enhanced)
        return rest == "native-pass-through" ? "room-dereverb" : "room-dereverb-and-\(rest)"
    }
    if enhanced != 0 {
        let name = EnhancedSuppression.fromNative(enhanced).rawValue
        return webRTCLevel >= 0 ? "webrtc-and-\(name)" : name
    }
    return webRTCLevel >= 0 ? "webrtc-noise-suppression" : "native-pass-through"
}

func nativeString<T>(_ value: T) -> String {
    var copy = value
    return withUnsafePointer(to: &copy) {
        $0.withMemoryRebound(to: CChar.self, capacity: MemoryLayout<T>.size) { String(cString: $0) }
    }
}

func deepFilterEngineLabel(_ stats: MVStats) -> String {
    switch stats.deepfilter_backend {
    case 1: return "Native CPU"
    case 3: return "Native CPU (Core ML unavailable)"
    case 2:
        let devices = [(UInt32(1), "CPU"), (UInt32(2), "GPU"), (UInt32(4), "Neural Engine")]
            .filter { stats.deepfilter_compute_devices & $0.0 != 0 }.map { $0.1 }
        return devices.isEmpty ? "Core ML · automatic" : "Core ML · \(devices.joined(separator: " + "))"
    default: return "Not running"
    }
}

func dpdfEngineLabel(_ stats: MVStats) -> String {
    guard stats.running != 0 && stats.enhanced_suppression == 3 else { return "Not running" }
    let devices = [(UInt32(1), "CPU"), (UInt32(2), "GPU"), (UInt32(4), "Neural Engine")]
        .filter { stats.dpdf_compute_devices & $0.0 != 0 }.map { $0.1 }
    return devices.isEmpty ? "Core ML · automatic" : "Core ML · \(devices.joined(separator: " + "))"
}

extension AudioDevice {
    static func list() -> [AudioDevice]? {
        let count = mv_devices(nil, 0)
        guard count >= 0 else { return nil }
        if count == 0 { return [] }
        var devices = [MVDevice](repeating: MVDevice(), count: Int(count) + 16)
        let found = mv_devices(&devices, Int32(devices.count))
        guard found >= 0 else { return nil }
        return devices.prefix(Int(found)).filter { $0.alive != 0 }.map {
            AudioDevice(id: $0.id, uid: nativeString($0.uid), name: nativeString($0.name),
                        inputs: Int($0.input_channels), outputs: Int($0.output_channels),
                        sampleRate: $0.sample_rate, bufferFrames: $0.buffer_frames,
                        isVirtual: $0.is_virtual != 0, isDefaultInput: $0.is_default_input != 0)
        }
    }
}

// All engine lifecycle/statistics calls use one control queue. Audio callbacks live in C++.
final class AudioController: ObservableObject {
    let voices = VoiceLibrary()
    @Published private(set) var cleanupEnabled = false
    @Published private(set) var voiceEnabled = false
    @Published var voiceOnLaunch = UserDefaults.standard.bool(forKey: "rvcOnLaunch") {
        didSet { UserDefaults.standard.set(voiceOnLaunch, forKey: "rvcOnLaunch") }
    }
    @Published private(set) var activeVoicePreset: VoicePreset?
    @Published var devices: [AudioDevice] = []
    @Published var inputUID = ""
    @Published var outputUID = ""
    @Published var channel = 0
    @Published var running = false
    @Published var busy = false
    @Published var muted = false
    @Published var status = "Ready"
    @Published var error: String?
    @Published var stats = MVStats()
    @Published var panelVisible = false
    @Published var showDetails = false
    @Published var enableOnLaunch = UserDefaults.standard.bool(forKey: "enableOnLaunch") {
        didSet { UserDefaults.standard.set(enableOnLaunch, forKey: "enableOnLaunch") }
    }
    @Published private(set) var echoCancellation = false
    @Published private(set) var roomDereverb = false
    @Published private(set) var noiseSuppression = false
    @Published private(set) var suppressionLevel = 1
    @Published private(set) var enhancedSuppression: EnhancedSuppression = .off
    @Published private var recovery = DeviceRecovery()
    var wantedRunning: Bool { recovery.requested }

    var inputs: [AudioDevice] { devices.filter(\.isMicrophone) }
    var outputs: [AudioDevice] { devices.filter { $0.outputs > 0 && $0.isVirtual } }
    var selectedInput: AudioDevice? { inputs.first { $0.uid == inputUID } }
    var selectedOutput: AudioDevice? { outputs.first { $0.uid == outputUID } }
    var hasVoice: Bool { voiceEnabled && voices.selected != nil }
    private var needsStream: Bool { cleanupEnabled || hasVoice }
    var canChooseRoute: Bool { needsStream && !busy }
    var suppressionSupported: Bool { selectedInput?.sampleRate == 48000 }
    var webRTCSelection: Int { noiseSuppression ? suppressionLevel : -1 }
    var activeWebRTCLevel: Int { cleanupEnabled ? webRTCSelection : -1 }
    var activeEnhanced: EnhancedSuppression { cleanupEnabled ? enhancedSuppression : .off }
    var separateRoomControlsAvailable: Bool { enhancedSuppression != .localVQE }
    var effectiveEchoCancellation: Bool { cleanupEnabled && echoCancellation && separateRoomControlsAvailable }
    var effectiveRoomDereverb: Bool { cleanupEnabled && roomDereverb && separateRoomControlsAvailable }
    var processingEnabled: Bool { effectiveEchoCancellation || effectiveRoomDereverb || activeWebRTCLevel >= 0 || activeEnhanced != .off }
    var processingLabel: String {
        let stages = (effectiveEchoCancellation ? ["Echo (AEC3)"] : []) + (effectiveRoomDereverb ? ["Room de-reverb"] : [])
            + (activeWebRTCLevel >= 0 ? ["WebRTC"] : []) + (activeEnhanced != .off ? [activeEnhanced.label] : [])
        return stages.isEmpty ? "Pass-through · suppression off" : stages.joined(separator: " + ")
    }
    private var activeStatus: String {
        if muted { return "Muted" }
        if hasVoice { return "Converting voice" }
        if effectiveEchoCancellation { return effectiveRoomDereverb || activeWebRTCLevel >= 0 || activeEnhanced != .off ? "Reducing echo + noise" : "Reducing speaker echo" }
        if effectiveRoomDereverb || activeEnhanced == .localVQE { return "Reducing noise + room reverb" }
        return processingEnabled ? "Reducing background noise" : "Passing microphone audio"
    }
    var echoReferenceStatus: String {
        guard running else { return "Not running" }
        switch stats.echo_reference_state {
        case 2: return stats.echo_reference_peak > 0.00001 ? "Synchronized" : "Synchronized · quiet playback"
        case 3: return "Unavailable"
        default: return "Waiting for playback reference"
        }
    }

    private let engine = mv_create()!
    private let control = DispatchQueue(label: "app.metalvoice2.control", qos: .userInitiated)
    private var timer: Timer?
    private var sleeping = false
    private var generation = 0
    private var pollPending = false
    private var inventoryGeneration: UInt64?
    private var previousDefaultUID = UserDefaults.standard.string(forKey: "lastDefaultInputUID")
    private var unchangedPolls = 0
    private var lastFrames: UInt64 = 0
    private var lastOutputFrames: UInt64 = 0
    private var observers: [NSObjectProtocol] = []

    init() {
        let defaults = UserDefaults.standard
        voiceEnabled = voiceOnLaunch && voices.selected != nil
        cleanupEnabled = enableOnLaunch
        inputUID = defaults.string(forKey: "inputUID") ?? ""
        outputUID = defaults.string(forKey: "outputUID") ?? ""
        channel = defaults.integer(forKey: "inputChannel")
        echoCancellation = defaults.bool(forKey: "echoCancellation")
        roomDereverb = defaults.bool(forKey: "roomDereverb")
        noiseSuppression = defaults.bool(forKey: "noiseSuppression")
        suppressionLevel = min(3, max(0, defaults.object(forKey: "suppressionLevel") as? Int ?? 1))
        enhancedSuppression = EnhancedSuppression(rawValue: defaults.string(forKey: "enhancedSuppression") ?? "off") ?? .off
        refreshDevices()
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in self?.poll() }
        let center = NSWorkspace.shared.notificationCenter
        observers.append(center.addObserver(forName: NSWorkspace.willSleepNotification, object: nil, queue: .main) { [weak self] _ in
            guard let self else { return }
            self.sleeping = true
            self.stop(preserveIntent: true)
            self.status = "Sleeping"
        })
        observers.append(center.addObserver(forName: NSWorkspace.didWakeNotification, object: nil, queue: .main) { [weak self] _ in
            guard let self else { return }
            self.sleeping = false
            self.refreshDevices()
            if self.wantedRunning { self.recovery.retryAt = Date().addingTimeInterval(1); self.status = "Reconnecting…" }
            else { self.status = "Ready" }
        })
    }

    deinit {
        timer?.invalidate()
        for observer in observers { NSWorkspace.shared.notificationCenter.removeObserver(observer) }
        control.sync { mv_destroy(engine) }
    }

    func meterLevels() -> MVMeterLevels { mv_meter_levels(engine) }

    func refreshDevices() {
        // Read the generation first so a change during enumeration triggers another refresh.
        let generation = mv_device_list_generation(engine)
        guard let available = AudioDevice.list() else {
            inventoryGeneration = nil
            return
        }
        applyDevices(available, generation: generation)
    }

    private func applyDevices(_ available: [AudioDevice], generation: UInt64) {
        let microphone = AudioDevice.microphone(in: available, selectedUID: inputUID, previousDefaultUID: previousDefaultUID)
        let reportedDefault = available.first(where: \.isDefaultInput)
        // HAL can announce a default before it publishes that microphone's channels.
        let defaultPending = reportedDefault.map { !$0.isVirtual && !$0.isMicrophone } ?? false
        inventoryGeneration = defaultPending ? nil : generation
        let defaultUID = reportedDefault?.uid ?? ""
        if devices != available { devices = available }
        if let microphone {
            let nextChannel = microphone.uid == inputUID ? min(max(0, channel), microphone.inputs - 1) : 0
            if microphone.uid != inputUID || nextChannel != channel {
                inputUID = microphone.uid
                channel = nextChannel
                saveSelection()
            }
        }
        if !defaultPending && previousDefaultUID != defaultUID {
            previousDefaultUID = defaultUID
            UserDefaults.standard.set(defaultUID, forKey: "lastDefaultInputUID")
        }
        if outputUID.isEmpty {
            outputUID = outputs.first(where: { $0.name.localizedCaseInsensitiveContains("BlackHole 2ch") })?.uid
                ?? outputs.first(where: { $0.name.localizedCaseInsensitiveContains("BlackHole") })?.uid ?? ""
        }
    }

    func chooseInput(_ uid: String) {
        guard canChooseRoute, uid != inputUID else { return }
        inputUID = uid
        channel = 0
        saveSelection()
        updateStream()
    }
    func chooseOutput(_ uid: String) {
        guard canChooseRoute, uid != outputUID else { return }
        outputUID = uid
        saveSelection()
        updateStream()
    }
    func chooseChannel(_ index: Int) {
        guard canChooseRoute, index != channel, (0..<(selectedInput?.inputs ?? 0)).contains(index) else { return }
        channel = index
        saveSelection()
        updateStream()
    }
    func saveSelection() {
        UserDefaults.standard.set(inputUID, forKey: "inputUID")
        UserDefaults.standard.set(outputUID, forKey: "outputUID")
        UserDefaults.standard.set(channel, forKey: "inputChannel")
    }

    func setWebRTCSelection(_ selection: Int) {
        guard (-1...3).contains(selection), selection != webRTCSelection, !busy else { return }
        guard selection == -1 || suppressionSupported else { return }
        // Update both persisted values together, then restart at most once.
        // Off retains the previous strength for compatibility with saved settings.
        noiseSuppression = selection >= 0
        if selection >= 0 { suppressionLevel = selection }
        UserDefaults.standard.set(noiseSuppression, forKey: "noiseSuppression")
        UserDefaults.standard.set(suppressionLevel, forKey: "suppressionLevel")
        if cleanupEnabled { updateStream() }
    }

    func setEchoCancellation(_ enabled: Bool) {
        guard separateRoomControlsAvailable, enabled != echoCancellation, !busy, !enabled || suppressionSupported else { return }
        echoCancellation = enabled
        UserDefaults.standard.set(enabled, forKey: "echoCancellation")
        if cleanupEnabled { updateStream() }
    }

    func setRoomDereverb(_ enabled: Bool) {
        guard separateRoomControlsAvailable, enabled != roomDereverb, !busy, !enabled || suppressionSupported else { return }
        roomDereverb = enabled
        UserDefaults.standard.set(enabled, forKey: "roomDereverb")
        if cleanupEnabled { updateStream() }
    }

    func setEnhancedSuppression(_ mode: EnhancedSuppression) {
        guard mode != enhancedSuppression, !busy, mode == .off || suppressionSupported else { return }
        enhancedSuppression = mode
        UserDefaults.standard.set(mode.rawValue, forKey: "enhancedSuppression")
        if cleanupEnabled { updateStream() }
    }

    func setCleanupEnabled(_ enabled: Bool) {
        guard !busy, enabled != cleanupEnabled else { return }
        cleanupEnabled = enabled
        updateStream(reconfigure: webRTCSelection >= 0 || enhancedSuppression != .off || echoCancellation || roomDereverb)
    }

    func setVoiceEnabled(_ enabled: Bool) {
        guard !busy, voiceEnabled != enabled else { return }
        voiceEnabled = enabled
        updateStream(reconfigure: voices.selected != nil)
    }

    func reloadVoice() {
        if voiceEnabled { updateStream() }
    }

    private func updateStream(reconfigure: Bool = true) {
        guard !busy else { return }
        error = nil
        guard needsStream else {
            if running || wantedRunning { stop() }
            else { status = "Stopped" }
            return
        }
        if !running {
            start()
            return
        }
        guard reconfigure else { return }
        // Reconfigure off the callbacks. Brief silence avoids mixing raw and
        // filtered audio with different algorithmic delay during a switch.
        stop(preserveIntent: true, restartImmediately: true)
    }

    func start() {
        guard needsStream, !busy, !running else { return }
        recovery.request()
        error = nil
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized: startAuthorized()
        case .notDetermined:
            busy = true; status = "Allow microphone access…"
            AVCaptureDevice.requestAccess(for: .audio) { [weak self] allowed in
                DispatchQueue.main.async {
                    guard let self else { return }
                    self.busy = false
                    guard self.wantedRunning else { return }
                    if allowed { self.startAuthorized() } else { self.permissionDenied() }
                }
            }
        default: permissionDenied()
        }
    }

    private func permissionDenied() {
        recovery.cancel(); status = "Microphone permission needed"
        error = "Allow MetalVoice2 in System Settings → Privacy & Security → Microphone, then turn Enable off and on."
    }

    private func startAuthorized() {
        guard !sleeping else { return }
        refreshDevices()
        guard let input = selectedInput else {
            status = "Waiting for a microphone"
            error = nil
            recovery.waitForDevice()
            return
        }
        guard let output = selectedOutput else {
            status = "Waiting for virtual output"
            error = outputs.isEmpty ? "Install or enable a virtual output such as BlackHole."
                : "Reconnect the selected virtual output, or choose another one."
            recovery.waitForDevice()
            return
        }
        if (processingEnabled || hasVoice) && input.sampleRate != 48000 {
            status = "Waiting for a 48 kHz microphone"
            error = "Set \(input.name) to 48,000 Hz in Audio MIDI Setup, or choose another microphone."
            recovery.waitForDevice()
            return
        }
        error = nil
        saveSelection()
        var launch: VoiceLaunch?
        if hasVoice {
            do { launch = try VoiceLaunch(voice: voices.selected) }
            catch {
                recovery.cancel()
                status = "Voice unavailable"
                self.error = error.localizedDescription
                return
            }
        }
        let voice = launch, preset = hasVoice ? voices.selected?.preset : nil
        busy = true; status = hasVoice ? "Loading voice…" : "Starting…"; generation += 1
        let token = generation, selectedChannel = UInt32(channel), muted = self.muted
        let nsLevel = Int32(activeWebRTCLevel)
        let enhancedMode = activeEnhanced.nativeMode
        let dereverb = effectiveRoomDereverb, echo = effectiveEchoCancellation
        control.async { [weak self] in
            guard let self else { return }
            mv_set_muted(self.engine, muted ? 1 : 0)
            mv_set_noise_suppression(self.engine, nsLevel)
            mv_set_echo_cancellation(self.engine, echo ? 1 : 0)
            mv_set_room_dereverb(self.engine, dereverb ? 1 : 0)
            mv_set_enhanced_suppression(self.engine, enhancedMode)
            var message = [CChar](repeating: 0, count: 1024)
            mv_set_voice(self.engine, voice?.python, voice?.worker, voice?.config, UInt32(preset?.chunk ?? 0))
            let result = mv_start(self.engine, input.id, output.id, selectedChannel, &message, message.count)
            let failure = String(cString: message)
            DispatchQueue.main.async {
                guard token == self.generation else { return }
                self.busy = false
                self.activeVoicePreset = result == 0 ? preset : nil
                self.running = result == 0
                self.status = result == 0 ? self.activeStatus : "Could not start"
                self.error = result == 0 ? nil : failure
                self.lastFrames = 0; self.lastOutputFrames = 0; self.unchangedPolls = 0
                self.recovery.completedStart(result)
                if result == MVStartDeviceNotReady { self.status = "Waiting for selected devices" }
            }
        }
    }

    func stop(preserveIntent: Bool = false, restartImmediately: Bool = false) {
        if !preserveIntent { recovery.cancel() }
        generation += 1
        let token = generation
        busy = true
        control.async { [weak self] in
            guard let self else { return }
            mv_stop(self.engine)
            DispatchQueue.main.async {
                guard token == self.generation else { return }
                self.running = false; self.busy = false; self.stats = MVStats()
                self.status = self.sleeping ? "Sleeping" : (self.wantedRunning ? "Reconnecting…" : "Stopped")
                if restartImmediately && self.wantedRunning && !self.sleeping { self.startAuthorized() }
            }
        }
    }

    func toggleMute() {
        muted.toggle()
        if running { status = activeStatus }
        let mute = muted
        control.async { [weak self] in if let self { mv_set_muted(self.engine, mute ? 1 : 0) } }
    }

    func copyReport() {
        guard running, !busy else { return }
        let token = generation
        control.async { [weak self] in
            guard let self else { return }
            var snapshot = MVStats()
            mv_stats(self.engine, &snapshot)
            DispatchQueue.main.async {
                guard token == self.generation, self.running else { return }
                self.writeReport(snapshot)
            }
        }
    }

    private func writeReport(_ stats: MVStats) {
        let report: [String: Any] = [
            "version": appVersion, "mode": suppressionModeName(webRTCLevel: stats.noise_suppression_level, enhanced: stats.enhanced_suppression, roomDereverb: stats.room_dereverb != 0, echo: stats.echo_cancellation != 0),
            "noiseSuppressionLevel": stats.noise_suppression_level,
            "echoCancellation": stats.echo_cancellation != 0,
            "echoAlgorithm": "WebRTC AEC3 · webrtc-audio-processing 2.1",
            "echoAlgorithmDelayMs": stats.echo_delay_ms,
            "echoReferenceHeadroomMs": stats.echo_reference_headroom_ms,
            "echoReferenceDevice": nativeString(stats.echo_reference_device),
            "echoReferenceState": stats.echo_reference_state,
            "echoReferenceFrames": stats.echo_reference_frames,
            "echoReferenceDrops": stats.echo_reference_drops,
            "echoReferenceAgeMs": stats.echo_reference_age_ms,
            "echoEstimatedERLEdB": stats.echo_erle_db >= 0 ? stats.echo_erle_db as Any : NSNull(),
            "echoEstimatedPathDelayMs": stats.echo_estimated_path_delay_ms >= 0 ? stats.echo_estimated_path_delay_ms as Any : NSNull(),
            "roomDereverb": stats.room_dereverb != 0,
            "roomDereverbAlgorithmDelayMs": stats.room_dereverb_delay_ms,
            "enhancedSuppression": EnhancedSuppression.fromNative(stats.enhanced_suppression).rawValue,
            "webRTCAlgorithmDelayMs": stats.webrtc_delay_ms, "rnnoiseAlgorithmDelayMs": stats.rnnoise_delay_ms,
            "deepFilterNet3AlgorithmDelayMs": stats.deepfilter_delay_ms,
            "deepFilterNet3Engine": deepFilterEngineLabel(stats),
            "deepFilterNet3ComputeDeviceMask": stats.deepfilter_compute_devices,
            "dpdfNet2AlgorithmDelayMs": stats.dpdf_delay_ms,
            "dpdfNet2Engine": dpdfEngineLabel(stats),
            "dpdfNet2ComputeDeviceMask": stats.dpdf_compute_devices,
            "localVQEModel": stats.enhanced_suppression == 4 ? "joint v1.3 · 4.8M · F32 · 16 kHz" : "Inactive",
            "localVQEThreads": stats.localvqe_threads,
            "localVQEAlgorithmDelayMs": stats.localvqe_delay_ms,
            "localVQEPlaybackReference": "None (noise + room cleanup)",
            "suppressionAlgorithmDelayMs": stats.suppression_delay_ms,
            "suppressionMeanComputeMs": stats.suppression_mean_ms, "suppressionMaxComputeMs": stats.suppression_max_ms,
            "suppressionP99ComputeMs": stats.suppression_p99_ms,
            "suppressionBlocks": stats.suppression_blocks,
            "latencyBoundary": "Core Audio capture timestamp to scheduled output timestamp, including suppression algorithm delay; not physical or receiver loopback",
            "microphone": selectedInput?.name ?? "Unavailable", "inputChannel": channel + 1,
            "virtualOutput": selectedOutput?.name ?? "Unavailable",
            "inputRateHz": stats.input_rate, "outputRateHz": stats.output_rate,
            "inputBufferFrames": stats.input_buffer_frames, "outputBufferFrames": stats.output_buffer_frames,
            "capturedFrames": stats.input_frames, "renderedFrames": stats.output_frames,
            "latencyValid": stats.latency_valid != 0,
            "latencyCurrentMs": stats.latency_valid != 0 ? stats.latency_ms as Any : NSNull(),
            "latencyP50Ms": stats.latency_samples > 0 ? stats.latency_p50_ms as Any : NSNull(),
            "latencyP95Ms": stats.latency_samples > 0 ? stats.latency_p95_ms as Any : NSNull(),
            "latencyMaxMs": stats.latency_samples > 0 ? stats.latency_max_ms as Any : NSNull(),
            "latencyCallbackSamples": stats.latency_samples,
            "queueMs": stats.queue_ms, "targetQueueMs": stats.target_queue_ms,
            "clockCorrectionPPM": stats.drift_ppm,
            "underruns": stats.underruns, "overruns": stats.overruns, "resyncs": stats.resyncs
        ]
        guard let data = try? JSONSerialization.data(withJSONObject: report, options: [.prettyPrinted, .sortedKeys]),
              let text = String(data: data, encoding: .utf8) else { return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
    }

    private func poll() {
        guard !busy, !sleeping, !pollPending else { return }
        if !running {
            if mv_device_list_generation(engine) != inventoryGeneration {
                let previousInput = selectedInput, previousOutput = selectedOutput
                refreshDevices()
                if wantedRunning && (previousInput != selectedInput || previousOutput != selectedOutput) {
                    recovery.retryAt = .distantPast
                }
            }
            if recovery.shouldRetry() { start() }
            return
        }
        pollPending = true
        let token = generation, lastInventory = inventoryGeneration
        control.async { [weak self] in
            guard let self else { return }
            var snapshot = MVStats()
            mv_stats(self.engine, &snapshot)
            let inventory = lastInventory != snapshot.device_list_generation ? AudioDevice.list() : nil
            DispatchQueue.main.async {
                self.pollPending = false
                guard token == self.generation else { return }
                let inputUID = self.inputUID, channel = self.channel
                if let inventory {
                    self.applyDevices(inventory, generation: snapshot.device_list_generation)
                }
                if snapshot.device_changed != 0 || inputUID != self.inputUID || channel != self.channel {
                    self.stop(preserveIntent: true, restartImmediately: true)
                    return
                }
                if snapshot.input_frames == self.lastFrames || snapshot.output_frames == self.lastOutputFrames {
                    self.unchangedPolls += 1
                } else { self.unchangedPolls = 0 }
                self.lastFrames = snapshot.input_frames
                self.lastOutputFrames = snapshot.output_frames
                if snapshot.last_error == MVErrorVoiceStream {
                    self.error = "Voice conversion stopped after a gap or missed deadline. Try a larger Chunk or shorter Extra history, then apply the settings."
                    self.stop()
                    return
                }
                if snapshot.last_error == MVErrorEchoReferenceUnavailable || snapshot.last_error == MVErrorEchoProcessing {
                    self.error = snapshot.last_error == MVErrorEchoReferenceUnavailable
                        ? "Echo lost its playback reference. Check System Audio Recording permission and the macOS sound output, then turn Enable off and on. Your calling app must play through that output."
                        : "Echo processing failed. Toggle Echo to retry or leave it off."
                    self.stop()
                    return
                }
                if snapshot.last_error != 0 || self.unchangedPolls >= 4 {
                    self.error = snapshot.last_error == 0 ? "Audio stalled. Reconnecting…"
                        : "Core Audio error \(snapshot.last_error); reconnecting."
                    self.recovery.retryAt = Date().addingTimeInterval(1)
                    self.stop(preserveIntent: true)
                    return
                }
                if self.panelVisible { self.stats = snapshot }
            }
        }
    }

    func shutdown() {
        recovery.cancel()
        timer?.invalidate()
        control.sync { mv_stop(engine) }
    }
}
