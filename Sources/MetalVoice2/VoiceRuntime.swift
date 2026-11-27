import Foundation

enum VoiceRuntime {
    static func prepare(in support: URL, using script: URL) throws -> String {
        let fs = FileManager.default
        try fs.createDirectory(at: support, withIntermediateDirectories: true)
        let log = support.appendingPathComponent("voice-setup.log")
        if !fs.fileExists(atPath: log.path) { fs.createFile(atPath: log.path, contents: nil) }
        let diagnostics = try FileHandle(forWritingTo: log)
        defer { try? diagnostics.close() }
        let process = Process()
        let output = Pipe()
        process.executableURL = URL(fileURLWithPath: "/bin/bash")
        process.arguments = [script.path, support.path]
        process.environment = ["PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "HOME": fs.homeDirectoryForCurrentUser.path]
        process.standardOutput = output
        process.standardError = diagnostics
        try process.run()
        let data = output.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        let python = String(decoding: data, as: UTF8.self).trimmingCharacters(in: .newlines)
        guard process.terminationStatus == 0, fs.isExecutableFile(atPath: python) else {
            throw NSError(domain: "MetalVoice2.VoiceRuntime", code: 1, userInfo: [NSLocalizedDescriptionKey:
                "Voice setup failed. Check your internet connection and available disk space, then enable Voice Changer again. Details: \(log.path)"])
        }
        return python
    }
}
