import Foundation
import CoreML

guard CommandLine.arguments.count == 3 else {
    fputs("Usage: compile-coreml INPUT.mlpackage OUTPUT.mlmodelc\n", stderr)
    exit(2)
}
let source = URL(fileURLWithPath: CommandLine.arguments[1])
let destination = URL(fileURLWithPath: CommandLine.arguments[2])
let compiled = try MLModel.compileModel(at: source)
defer { try? FileManager.default.removeItem(at: compiled) }
if FileManager.default.fileExists(atPath: destination.path) {
    try FileManager.default.removeItem(at: destination)
}
try FileManager.default.copyItem(at: compiled, to: destination)
print("Compiled \(destination.path)")
