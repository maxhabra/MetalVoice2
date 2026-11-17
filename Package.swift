// swift-tools-version: 6.0
import PackageDescription
import Foundation

let deepFilterLibraryPath = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    .appendingPathComponent("build/deepfilter/release").path

let echoLibraryPath = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    .appendingPathComponent("build/aec-native").path
let localVQELibraryPath = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    .appendingPathComponent("build/localvqe-native").path

let package = Package(
    name: "MetalVoice2",
    platforms: [.macOS(.v15)],
    products: [
        .executable(name: "MetalVoice2", targets: ["MetalVoice2"]),
        .executable(name: "transport-tests", targets: ["TransportTests"]),
        .executable(name: "noise-tests", targets: ["NoiseTests"]),
        .executable(name: "coreml-tests", targets: ["CoreMLTests"]),
        .executable(name: "dpdf-tests", targets: ["DPDFTests"]),
        .executable(name: "room-tests", targets: ["RoomTests"]),
        .executable(name: "aec-tests", targets: ["AECTests"]),
        .executable(name: "localvqe-tests", targets: ["LocalVQETests"])
    ],
    targets: [
        .target(name: "RNNoise", path: "ThirdParty/RNNoise/upstream", sources: [
            "src/denoise.c", "src/rnn.c", "src/pitch.c", "src/kiss_fft.c",
            "src/celt_lpc.c", "src/nnet.c", "src/nnet_default.c",
            "src/parse_lpcnet_weights.c", "src/rnnoise_data.c", "src/rnnoise_tables.c"
        ], publicHeadersPath: "include", cSettings: [.headerSearchPath("src")]),
        .target(name: "WebRTCNS", path: "ThirdParty/WebRTC", sources: [
            "wrapper.cpp", "upstream/modules/audio_processing/ns",
            "upstream/modules/audio_processing/three_band_filter_bank.cc",
            "upstream/common_audio/third_party/ooura/fft_size_256/fft4g.cc"
        ], publicHeadersPath: "include", cxxSettings: [
            .define("webrtc", to: "mv_webrtc_ns"),
            .headerSearchPath("compat"), .headerSearchPath("upstream")
        ]),
        .target(name: "WebRTCAEC", path: "ThirdParty/WebRTCAEC", sources: ["wrapper.cpp"], publicHeadersPath: "include", cxxSettings: [
            .headerSearchPath("webrtc-audio-processing-2.1/webrtc"),
            .headerSearchPath("webrtc-audio-processing-2.1/subprojects/abseil-cpp-20240722.0"),
            .define("WEBRTC_MAC"), .define("WEBRTC_POSIX")
        ], linkerSettings: [.unsafeFlags(["-L", echoLibraryPath]), .linkedLibrary("mv_webrtc_aec"), .linkedFramework("CoreFoundation")]),
        .target(name: "PlaybackCapture", publicHeadersPath: "include", cxxSettings: [.unsafeFlags(["-fobjc-arc"])], linkerSettings: [
            .linkedFramework("Foundation"), .linkedFramework("CoreAudio")
        ]),
        .target(name: "AppleDF", publicHeadersPath: "include", cxxSettings: [.unsafeFlags(["-fobjc-arc"])], linkerSettings: [
            .linkedFramework("CoreML"), .linkedFramework("Foundation"), .linkedFramework("Accelerate")
        ]),
        .target(name: "LocalVQE", publicHeadersPath: "include", cxxSettings: [
            .unsafeFlags(["-I", "ThirdParty/LocalVQE/upstream/ggml"])
        ], linkerSettings: [
            .unsafeFlags(["-L", localVQELibraryPath]), .linkedLibrary("mv_localvqe"),
            .linkedFramework("CoreFoundation"), .linkedFramework("Accelerate")
        ]),
        .target(name: "AudioCore", dependencies: ["WebRTCNS", "RNNoise", "AppleDF", "WebRTCAEC", "PlaybackCapture", "LocalVQE"], publicHeadersPath: "include", linkerSettings: [
            .linkedFramework("CoreAudio"), .linkedFramework("AudioToolbox"), .linkedFramework("CoreFoundation"),
            .unsafeFlags(["-L", deepFilterLibraryPath]), .linkedLibrary("metalvoice_df")
        ]),
        .executableTarget(name: "MetalVoice2", dependencies: ["AudioCore"], swiftSettings: [.swiftLanguageMode(.v5)], linkerSettings: [
            .linkedFramework("AppKit"), .linkedFramework("SwiftUI"), .linkedFramework("AVFoundation")
        ]),
        .executableTarget(name: "TransportTests", dependencies: ["AudioCore"], path: "Tests/TransportTests"),
        .executableTarget(name: "NoiseTests", dependencies: ["AudioCore", "WebRTCNS", "RNNoise"], path: "Tests/NoiseTests"),
        .executableTarget(name: "CoreMLTests", dependencies: ["AudioCore", "AppleDF", "WebRTCNS"], path: "Tests/CoreMLTests"),
        .executableTarget(name: "AECTests", dependencies: ["AudioCore", "WebRTCAEC", "PlaybackCapture"], path: "Tests/AECTests"),
        .executableTarget(name: "LocalVQETests", dependencies: ["AudioCore", "LocalVQE", "WebRTCNS"], path: "Tests/LocalVQETests", cxxSettings: [
            .unsafeFlags(["-I", "ThirdParty/LocalVQE/upstream/ggml"])
        ]),
        .executableTarget(name: "RoomTests", dependencies: ["AudioCore"], path: "Tests/RoomTests"),
        .executableTarget(name: "DPDFTests", dependencies: ["AudioCore", "AppleDF", "WebRTCNS"], path: "Tests/DPDFTests")
    ],
    cxxLanguageStandard: .cxx20
)
