import Foundation
import AVFoundation
import AudioCore
import AppKit
import Darwin

// Reproducible local run after the user has granted this app microphone access.
// Nothing records samples or saves microphone content.
func runBenchmark(arguments: [String]) -> Int32 {
    guard let index = arguments.firstIndex(of: "--benchmark"), index + 1 < arguments.count,
          let duration = Double(arguments[index + 1]), duration >= 1, duration <= 3600 else {
        fputs("Usage: MetalVoice2 --benchmark SECONDS [--input-uid UID] [--output-uid UID] [--ns off|low|moderate|high|very-high] [--enhanced off|rnnoise|deepfilternet3|dpdfnet2|localvqe] [--df-backend coreml|native] [--room-dereverb] [--echo]\n", stderr)
        return 2
    }
    guard AVCaptureDevice.authorizationStatus(for: .audio) == .authorized else {
        fputs("Microphone access has not been granted. Open MetalVoice2.app, enable the Microphone tab, and allow microphone access first.\n", stderr)
        return 3
    }
    if NSRunningApplication.runningApplications(withBundleIdentifier: "app.metalvoice2.baseline")
        .contains(where: { $0.processIdentifier != ProcessInfo.processInfo.processIdentifier }) {
        fputs("Quit the MetalVoice2 menu app before benchmarking to avoid two writers feeding the virtual microphone.\n", stderr)
        return 8
    }
    func option(_ name: String) -> String? {
        guard let i = arguments.firstIndex(of: name), i + 1 < arguments.count else { return nil }
        return arguments[i + 1]
    }
    let devices = AudioDevice.list()
    let inputUID = option("--input-uid"), outputUID = option("--output-uid")
    guard let input = devices.first(where: { $0.inputs > 0 && !$0.isVirtual && (inputUID != nil ? $0.uid == inputUID : $0.isDefaultInput) }),
          let output = devices.first(where: { $0.outputs > 0 && $0.isVirtual && (outputUID != nil ? $0.uid == outputUID : $0.name.localizedCaseInsensitiveContains("BlackHole 2ch")) }) else {
        fputs("Selected physical microphone or BlackHole output is unavailable. Use --list-devices.\n", stderr)
        return 4
    }
    let engine = mv_create()!
    defer { mv_destroy(engine) }
    let levels: [String: Int32] = ["off": -1, "low": 0, "moderate": 1, "high": 2, "very-high": 3]
    guard let nsLevel = levels[option("--ns") ?? "off"] else {
        fputs("Invalid --ns level. Use off, low, moderate, high, or very-high.\n", stderr)
        return 2
    }
    let echo = arguments.contains("--echo")
    mv_set_echo_cancellation(engine, echo ? 1 : 0)
    let roomDereverb = arguments.contains("--room-dereverb")
    mv_set_room_dereverb(engine, roomDereverb ? 1 : 0)
    mv_set_noise_suppression(engine, nsLevel)
    guard let enhanced = EnhancedSuppression(rawValue: option("--enhanced") ?? "off") else {
        fputs("Invalid --enhanced mode. Use off, rnnoise, deepfilternet3, dpdfnet2, or localvqe.\n", stderr)
        return 2
    }
    mv_set_enhanced_suppression(engine, enhanced.nativeMode)
    let dfBackend = option("--df-backend") ?? "coreml"
    guard ["coreml", "native"].contains(dfBackend) else {
        fputs("Invalid --df-backend. Use coreml or native.\n", stderr)
        return 2
    }
    mv_set_deepfilter_acceleration(engine, dfBackend == "coreml" ? 1 : 0)
    var error = [CChar](repeating: 0, count: 1024)
    guard mv_start(engine, input.id, output.id, 0, &error, error.count) == 0 else {
        fputs("\(String(cString: error))\n", stderr)
        return 5
    }
    func usage() -> rusage {
        var value = rusage()
        getrusage(RUSAGE_SELF, &value)
        return value
    }
    func cpuSeconds(_ value: rusage) -> Double {
        Double(value.ru_utime.tv_sec + value.ru_stime.tv_sec)
            + Double(value.ru_utime.tv_usec + value.ru_stime.tv_usec) / 1e6
    }
    let initialUsage = usage()
    let start = ProcessInfo.processInfo.systemUptime
    var stats = MVStats()
    repeat {
        RunLoop.current.run(until: Date().addingTimeInterval(min(0.25, duration)))
        mv_stats(engine, &stats)
        if stats.last_error != 0 || stats.device_changed != 0 {
            fputs("Audio route changed or failed during benchmark.\n", stderr)
            return 6
        }
    } while ProcessInfo.processInfo.systemUptime - start < duration
    let durationSeconds = ProcessInfo.processInfo.systemUptime - start
    let finalUsage = usage()
    mv_stop(engine)
    let report: [String: Any] = [
        "version": appVersion, "mode": suppressionModeName(webRTCLevel: nsLevel, enhanced: enhanced.nativeMode, roomDereverb: stats.room_dereverb != 0, echo: stats.echo_cancellation != 0),
        "noiseSuppressionLevel": stats.noise_suppression_level,
        "enhancedSuppression": enhanced.rawValue,
        "echoCancellation": stats.echo_cancellation != 0,
        "echoAlgorithmDelayMs": stats.echo_delay_ms,
        "echoReferenceHeadroomMs": stats.echo_reference_headroom_ms,
        "echoReferenceDevice": nativeString(stats.echo_reference_device),
        "echoReferenceState": stats.echo_reference_state,
        "echoReferenceFrames": stats.echo_reference_frames,
        "echoReferenceDrops": stats.echo_reference_drops,
        "echoEstimatedERLEdB": stats.echo_erle_db,
        "roomDereverb": stats.room_dereverb != 0,
        "roomDereverbAlgorithmDelayMs": stats.room_dereverb_delay_ms,
        "webRTCAlgorithmDelayMs": stats.webrtc_delay_ms, "rnnoiseAlgorithmDelayMs": stats.rnnoise_delay_ms,
        "deepFilterNet3AlgorithmDelayMs": stats.deepfilter_delay_ms,
        "deepFilterNet3Engine": deepFilterEngineLabel(stats),
        "deepFilterNet3ComputeDeviceMask": stats.deepfilter_compute_devices,
        "dpdfNet2AlgorithmDelayMs": stats.dpdf_delay_ms,
        "dpdfNet2Engine": dpdfEngineLabel(stats),
        "dpdfNet2ComputeDeviceMask": stats.dpdf_compute_devices,
        "localVQEAlgorithmDelayMs": stats.localvqe_delay_ms,
        "localVQEThreads": stats.localvqe_threads,
        "suppressionAlgorithmDelayMs": stats.suppression_delay_ms,
        "suppressionMeanComputeMs": stats.suppression_mean_ms, "suppressionMaxComputeMs": stats.suppression_max_ms,
        "suppressionP99ComputeMs": stats.suppression_p99_ms,
        "suppressionBlocks": stats.suppression_blocks,
        "durationSeconds": durationSeconds,
        "cpuPercentOfOneCore": (cpuSeconds(finalUsage) - cpuSeconds(initialUsage)) / durationSeconds * 100,
        "peakResidentBytes": finalUsage.ru_maxrss,
        "input": input.name, "output": output.name,
        "latencyBoundary": "capture timestamp to output timestamp, including suppression algorithm delay; not physical or receiver loopback",
        "latencyValid": stats.latency_valid != 0, "latencyCallbackSamples": stats.latency_samples,
        "queueMs": stats.queue_ms, "targetQueueMs": stats.target_queue_ms, "clockCorrectionPPM": stats.drift_ppm,
        "p50Ms": stats.latency_p50_ms, "p95Ms": stats.latency_p95_ms, "maxMs": stats.latency_max_ms,
        "inputFrames": stats.input_frames, "outputFrames": stats.output_frames,
        "underruns": stats.underruns, "overruns": stats.overruns, "resyncs": stats.resyncs,
        "inputRateHz": stats.input_rate, "outputRateHz": stats.output_rate,
        "inputBufferFrames": stats.input_buffer_frames, "outputBufferFrames": stats.output_buffer_frames
    ]
    if let data = try? JSONSerialization.data(withJSONObject: report, options: [.prettyPrinted, .sortedKeys]) {
        print(String(decoding: data, as: UTF8.self))
    }
    return stats.input_frames > 0 && stats.output_frames > 0 && stats.latency_samples > 0 && ((nsLevel < 0 && enhanced == .off && !roomDereverb && !echo) || stats.suppression_blocks > 0)
        && (stats.echo_cancellation == 0 || (stats.echo_reference_state == 2 && stats.echo_reference_frames > 0 && stats.echo_reference_drops == 0))
        && stats.underruns == 0 && stats.overruns == 0 && stats.resyncs == 0 ? 0 : 7
}
