import Foundation

let temporary = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
let suite = "VoiceLibraryTests." + UUID().uuidString
let defaults = UserDefaults(suiteName: suite)!
try FileManager.default.createDirectory(at: temporary, withIntermediateDirectories: true)
defer {
    try? FileManager.default.removeItem(at: temporary)
    defaults.removePersistentDomain(forName: suite)
}

func fixture(_ name: String) throws -> URL {
    let url = temporary.appendingPathComponent(name).standardizedFileURL
    try Data([1]).write(to: url)
    return url
}

let a = try fixture("A.pth"), b = try fixture("B.PTH"), index = try fixture("A.index")
let library = VoiceLibrary(defaults: defaults)
try library.add([a, b])
precondition(library.catalog.voices.count == 2 && library.selected?.model == b)
precondition(library.selected?.index == nil)
library.select(a)
precondition(library.selected?.index == index)
var tuned = library.selected!.preset
tuned.pitch = -3
tuned.highPass = 120
tuned.gateHold = 230
tuned.f0 = .harvest
library.setDraft(tuned)
precondition(library.hasEdits && library.selected!.preset == VoicePreset())
precondition(VoiceLibrary(defaults: defaults).selected!.preset == VoicePreset())
library.cancelDraft()
precondition(!library.hasEdits && library.editedPreset == VoicePreset())
library.setDraft(tuned)
library.select(b)
precondition(!library.hasEdits)
library.select(a)
precondition(library.editedPreset == tuned)
library.applyDraft()
precondition(!library.hasEdits)
var unsaved = tuned
unsaved.chunk = 200
library.setDraft(unsaved)
try library.add([a])
precondition(library.catalog.voices.count == 2 && library.selected?.preset == tuned)
let restored = VoiceLibrary(defaults: defaults)
precondition(restored.selected?.model == a && restored.selected?.preset == tuned)
restored.select(b)
precondition(restored.selected?.preset == VoicePreset())
try restored.attach(index)
precondition(restored.selected?.index == index)
try restored.attach(nil)
precondition(restored.selected?.index == nil)
let extra = try fixture("C.pth")
do {
    try restored.add([extra, index])
    fatalError("Wrong extension accepted")
} catch { precondition(restored.catalog.voices.count == 2 && restored.selected?.model == b) }
restored.removeSelected()
precondition(restored.selected == nil && restored.catalog.voices.count == 1 && FileManager.default.fileExists(atPath: b.path))
let historical = Data(#"{"pitch":1,"chunk":250,"extra":80,"indexStrength":50,"highPass":100,"lowPass":10000,"gate":-70,"f0":"rmvpe_onnx"}"#.utf8)
let migrated = try JSONDecoder().decode(VoicePreset.self, from: historical)
precondition(migrated.gateHold == 0 && migrated.pitch == 1 && migrated.f0 == .rmvpe)
print("PASS draft cancellation, applied-only persistence and legacy preset migration")
print("PASS voice import, auto/manual index pairing, per-model settings, persistence and removal")
