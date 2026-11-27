import Foundation
import Combine

enum F0Estimator: String, Codable {
    case rmvpe = "rmvpe_onnx"
    case harvest
}

struct VoicePreset: Codable, Equatable {
    var pitch = 10.0
    var chunk = 500.0
    var extra = 340.0
    var indexStrength = 50.0
    var highPass = 100.0
    var lowPass = 10_000.0
    var gate = -120.0
    // Optional backing keeps catalogs saved before gate hold was introduced readable.
    private var gateHoldMS: Double?
    var gateHold: Double {
        get { gateHoldMS ?? 0 }
        set { gateHoldMS = newValue == 0 ? nil : newValue }
    }
    private var speakerID: Int?
    var speaker: Int {
        get { speakerID ?? 0 }
        set { speakerID = newValue == 0 ? nil : newValue }
    }
    var f0: F0Estimator = .rmvpe
}

struct VoiceEntry: Identifiable, Codable {
    let model: URL
    var index: URL?
    var indexSHA256: String?
    var preset = VoicePreset()
    var speakerCount: Int?
    var id: URL { model }
    var name: String { model.deletingPathExtension().lastPathComponent }
}

// Store references to checkpoints. Registering a voice never deserializes model weights.
final class VoiceLibrary: ObservableObject {
    struct Catalog: Codable {
        var voices: [VoiceEntry] = []
        var selection: URL?
    }

    @Published private(set) var catalog: Catalog
    @Published private var drafts: [URL: VoicePreset] = [:]
    private let defaults: UserDefaults
    private let key = "rvcVoiceLibrary.v1"

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        catalog = defaults.data(forKey: key).flatMap { try? JSONDecoder().decode(Catalog.self, from: $0) } ?? Catalog()
    }

    var selected: VoiceEntry? { catalog.voices.first { $0.id == catalog.selection } }

    private func save(_ updated: Catalog) {
        guard let data = try? JSONEncoder().encode(updated) else { return }
        defaults.set(data, forKey: key)
        catalog = updated
    }

    private func checked(_ file: URL, extension ext: String) throws -> URL {
        let url = file.standardizedFileURL
        guard url.isFileURL, url.pathExtension.lowercased() == ext,
              try url.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile == true,
              FileManager.default.isReadableFile(atPath: url.path) else {
            throw CocoaError(.fileReadUnknown, userInfo: [NSLocalizedDescriptionKey: "Choose a readable .\(ext) file.", NSURLErrorKey: url])
        }
        return url
    }

    func add(_ files: [URL]) throws {
        var updated = catalog
        for file in files {
            let url = try checked(file, extension: "pth")
            if !updated.voices.contains(where: { $0.id == url }) {
                updated.voices.append(VoiceEntry(model: url))
            }
            updated.selection = url
        }
        save(updated)
    }

    func select(_ id: URL?) {
        guard id == nil || catalog.voices.contains(where: { $0.id == id }) else { return }
        var updated = catalog
        updated.selection = id
        save(updated)
    }

    func attach(_ file: URL?, sha256: String = "") throws {
        let index = try file.map { try checked($0, extension: "index") }
        let digest = sha256.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        if index != nil && !(digest.utf8.count == 64 && digest.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) }) {
            throw NSError(domain: "MetalVoice2.Index", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "Enter the 64-character SHA-256 from a trusted publisher."])
        }
        updateSelected {
            $0.index = index
            $0.indexSHA256 = index == nil ? nil : digest
        }
    }

    func setSpeakerCount(_ count: Int, for model: URL) {
        guard count > 0, selected?.model == model, selected?.speakerCount != count else { return }
        updateSelected { $0.speakerCount = count }
    }

    var editedPreset: VoicePreset? {
        guard let voice = selected else { return nil }
        return drafts[voice.id] ?? voice.preset
    }

    var hasEdits: Bool { editedPreset != selected?.preset }

    func setDraft(_ preset: VoicePreset) {
        guard let voice = selected, preset != editedPreset else { return }
        drafts[voice.id] = preset == voice.preset ? nil : preset
    }

    func cancelDraft() {
        if let id = catalog.selection { drafts.removeValue(forKey: id) }
    }

    func applyDraft() {
        guard let id = catalog.selection, let draft = drafts.removeValue(forKey: id) else { return }
        updateSelected { $0.preset = draft }
    }

    private func updateSelected(_ edit: (inout VoiceEntry) -> Void) {
        guard let i = catalog.voices.firstIndex(where: { $0.id == catalog.selection }) else { return }
        var updated = catalog
        edit(&updated.voices[i])
        save(updated)
    }

    func removeSelected() {
        cancelDraft()
        var updated = catalog
        updated.voices.removeAll { $0.id == catalog.selection }
        updated.selection = nil
        save(updated)
    }
}
