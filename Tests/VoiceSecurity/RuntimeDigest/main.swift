import Foundation

let fs = FileManager.default
let root = fs.temporaryDirectory.resolvingSymlinksInPath().appendingPathComponent(UUID().uuidString)
try fs.createDirectory(at: root, withIntermediateDirectories: true)
defer { try? fs.removeItem(at: root) }

let worker = root.appendingPathComponent("worker.py")
let link = root.appendingPathComponent("worker-link")
try Data("fixture".utf8).write(to: worker)
try fs.createSymbolicLink(atPath: link.path, withDestinationPath: worker.lastPathComponent)
let original = try VoiceRuntime.treeDigest(root)
precondition(original == CommandLine.arguments[1], "Runtime and build-time checks must agree")

try Data("changed fixture".utf8).write(to: worker)
let changed = try VoiceRuntime.treeDigest(root)
precondition(changed != original)
try fs.removeItem(at: link)
try fs.createSymbolicLink(atPath: link.path, withDestinationPath: "../")
do {
    _ = try VoiceRuntime.treeDigest(root)
    fatalError("Accepted a link outside the runtime")
} catch {}
print("PASS runtime seal binds worker bytes and rejects external links")
