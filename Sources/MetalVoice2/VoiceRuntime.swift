import CryptoKit
import Darwin
import MachO
import Foundation

// Only the audio control queue calls verify(). The package installs here through macOS Installer.
enum VoiceRuntime {
    static let root = URL(fileURLWithPath: "/Library/MetalVoice2/RVC-1", isDirectory: true)
    // Reuse a successful check across microphone handoffs until Installer replaces the package.
    private static var verifiedReceipt: timespec?
    private static func receiptChangeTime() -> timespec? {
        var info = stat()
        guard lstat("/var/db/receipts/app.metalvoice2.voice-runtime.bom", &info) == 0,
              info.st_uid == 0, info.st_mode & S_IFMT == S_IFREG else { return nil }
        return info.st_ctimespec
    }
    // CPython plus the exact bytes from runtime-requirements.txt, before any Python startup.
    static let pythonDigest = "1f4c1015b5702cce174a331621b1457b4cc3f13f4e33aedad21296b41958c9c1"

    private static func failure(_ reason: String) -> NSError {
        NSError(domain: "MetalVoice2.VoiceRuntime", code: 1, userInfo: [NSLocalizedDescriptionKey:
            "\(reason) Rebuild and install the voice runtime with setup_voice_runtime.sh."])
    }

    private static func checkPermissions(_ url: URL) throws {
        var info = stat()
        guard lstat(url.path, &info) == 0, info.st_uid == 0 else { throw failure("The voice runtime requires administrator ownership.") }
        if info.st_mode & S_IFMT == S_IFLNK { return } // Its containing directory and destination are checked.
        guard info.st_mode & 0o022 == 0 else { throw failure("The voice runtime is writable by another account.") }
        if let acl = acl_get_file(url.path, ACL_TYPE_EXTENDED) {
            defer { acl_free(UnsafeMutableRawPointer(acl)) }
            var entry: acl_entry_t?
            if acl_get_entry(acl, Int32(ACL_FIRST_ENTRY.rawValue), &entry) == 0 {
                throw failure("Unexpected access permissions on the voice runtime.")
            }
        } else if errno != ENOENT { throw failure("Could not verify voice runtime permissions.") }
    }

    static func treeDigest(_ directory: URL) throws -> String {
        let fs = FileManager.default
        var records = [(String, UInt8, SHA256.Digest)]()
        let canonicalRoot = directory.resolvingSymlinksInPath().standardizedFileURL.path
        func visit(_ folder: URL, prefix: String) throws {
            for item in try fs.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil) {
                let path = item.path
                let name = prefix + item.lastPathComponent
                var info = stat()
                guard lstat(path, &info) == 0 else { throw failure("Unreadable runtime file.") }
                switch info.st_mode & S_IFMT {
                case S_IFDIR: try visit(item, prefix: name + "/")
                case S_IFLNK:
                    let target = try fs.destinationOfSymbolicLink(atPath: path)
                    guard !target.hasPrefix("/"), item.resolvingSymlinksInPath().standardizedFileURL.path.hasPrefix(canonicalRoot + "/") else {
                        throw failure("A runtime link points outside the installation.")
                    }
                    records.append((name, 76, SHA256.hash(data: Data(target.utf8))))
                case S_IFREG:
                    records.append((name, 70, SHA256.hash(data: try Data(contentsOf: item, options: .mappedIfSafe))))
                default: throw failure("Unexpected file type in the voice runtime.")
                }
            }
        }
        try visit(directory, prefix: "")
        var digest = SHA256()
        for (name, type, hash) in records.sorted(by: { $0.0.utf8.lexicographicallyPrecedes($1.0.utf8) }) {
            digest.update(data: Data(name.utf8))
            digest.update(data: Data([0, type]))
            digest.update(data: Data(hash))
        }
        return digest.finalize().map { String(format: "%02x", $0) }.joined()
    }

    static func verify() throws {
        let receipt = receiptChangeTime()
        if let receipt, let cached = verifiedReceipt,
           receipt.tv_sec == cached.tv_sec && receipt.tv_nsec == cached.tv_nsec { return }
        // A symlink or writable parent could replace an otherwise authenticated tree.
        for path in ["/", "/Library", "/Library/MetalVoice2", root.path] {
            let ancestor = URL(fileURLWithPath: path, isDirectory: true)
            guard ancestor.resolvingSymlinksInPath().path == path else { throw failure("Unexpected runtime location.") }
            try checkPermissions(ancestor)
        }
        var enumerationFailed = false
        guard let entries = FileManager.default.enumerator(at: root, includingPropertiesForKeys: [.isSymbolicLinkKey], errorHandler: { _, _ in
            enumerationFailed = true
            return false
        }) else { throw failure("Voice runtime is missing.") }
        for case let file as URL in entries {
            try checkPermissions(file)
            if (try file.resourceValues(forKeys: [.isSymbolicLinkKey])).isSymbolicLink == true,
               !file.resolvingSymlinksInPath().path.hasPrefix(root.path + "/") { throw failure("External runtime link.") }
        }
        guard !enumerationFailed else { throw failure("Could not inspect the entire runtime.") }
        try authenticatePayload(at: root)
        verifiedReceipt = receipt
    }

    static func authenticatePayload(at directory: URL) throws {
        var size: UInt = 0
        guard let header = _dyld_get_image_header(0),
              let bytes = getsectiondata(UnsafeRawPointer(header).assumingMemoryBound(to: mach_header_64.self),
                                         "__TEXT", "__mv_rvcseal", &size), size == 64,
              try treeDigest(directory.appendingPathComponent("Contents")) == String(decoding: UnsafeBufferPointer(start: bytes, count: 64), as: UTF8.self),
              try treeDigest(directory.appendingPathComponent("python")) == pythonDigest
        else { throw failure("The installed runtime does not match this build of MetalVoice2.") }
    }
}
