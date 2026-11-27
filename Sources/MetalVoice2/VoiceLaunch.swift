import Foundation

struct VoiceLaunch {
    let python: String
    let worker: String
    let config: String

    private struct Configuration: Encodable {
        var model: String
        var index: String?
        var indexSHA256: String?
        var assets: String
        var preset: VoicePreset
        let reportSpeakerCount = true
    }

    init(voice: VoiceEntry?) throws {
        func failure(_ message: String) -> NSError {
            NSError(domain: "MetalVoice2.RVC", code: 1, userInfo: [NSLocalizedDescriptionKey: message])
        }
        guard let voice, FileManager.default.isReadableFile(atPath: voice.model.path) else {
            throw failure("Select a readable .pth voice or turn Voice changer off.")
        }
        let root = VoiceRuntime.root
        python = root.appendingPathComponent("python/bin/python3.11").path
        worker = root.appendingPathComponent("Contents/Resources/RVC/worker.py").path
        guard FileManager.default.isExecutableFile(atPath: python) else {
            throw failure("Install the protected voice runtime using setup_voice_runtime.sh.")
        }
        var folder = voice.model.deletingLastPathComponent()
        var candidates: [URL] = []
        if let chosen = UserDefaults.standard.string(forKey: "rvcAssetsFolder"), !chosen.isEmpty {
            candidates.append(URL(fileURLWithPath: chosen))
        }
        for _ in 0..<4 {
            candidates.append(folder.appendingPathComponent("modules"))
            folder.deleteLastPathComponent()
        }
        var requiredAssets = ["contentvec/contentvec-f.onnx"]
        if voice.preset.f0 == .rmvpe { requiredAssets.append("rmvpe/rmvpe_20231006.onnx") }
        guard let assets = candidates.first(where: { root in
            requiredAssets.allSatisfy {
                FileManager.default.isReadableFile(atPath: root.appendingPathComponent($0).path)
            }
        }) else {
            throw failure("Click Model assets… and select the modules folder from VCClient.")
        }
        let data = try JSONEncoder().encode(Configuration(model: voice.model.path,
                                                          index: voice.indexSHA256 == nil ? nil : voice.index?.path,
                                                          indexSHA256: voice.indexSHA256,
                                                          assets: assets.path, preset: voice.preset))
        config = String(decoding: data, as: UTF8.self)
    }
}
