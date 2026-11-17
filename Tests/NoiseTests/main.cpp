#include "mv_webrtc_ns.h"
#include "NoisePipeline.hpp"
#include "mv_audio.h"
#include "rnnoise.h"
#include "CaptureQueue.hpp"
#include "Transport.hpp"
#include <array>
#include <vector>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <random>
#include <chrono>
#include <cstdlib>
#include <new>
#include <limits>
#include <dlfcn.h>

static thread_local bool trackAllocations = false;
static thread_local size_t allocations = 0;
void* operator new(std::size_t size) {
    if (trackAllocations) ++allocations;
    if (void *p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }

// Cover upstream C allocation too, using a separate test-only dyld image.
static auto cTracking = reinterpret_cast<void (*)(int)>(dlsym(RTLD_DEFAULT, "mv_test_track_allocations"));
static auto cAllocationCount = reinterpret_cast<size_t (*)()>(dlsym(RTLD_DEFAULT, "mv_test_allocation_count"));
static size_t cAllocationBaseline = 0;
static void tracking(bool enabled) {
    trackAllocations = enabled;
    if (cTracking) cTracking(enabled);
}

static void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }

static void pipelineTests() {
#if !__has_feature(address_sanitizer)
    // Prove the C allocator hook is effective before trusting its zero count.
    require(cTracking && cAllocationCount, "Run noise-tests through scripts/test.sh to enable C allocation tracking");
    auto *(*volatile allocate)(size_t) = std::malloc;
    tracking(true);
    void *probe = allocate(19);
    tracking(false);
    std::free(probe);
    require(cAllocationCount() > 0, "C allocation tracking must be active");
    cAllocationBaseline = cAllocationCount();
    allocations = 0;
#endif
    mv::NoisePipeline pipeline;
    require(!pipeline.configure(-2, true) && !pipeline.configure(4, true), "Invalid pipeline levels must fail");
    auto *engine = mv_create();
    require(mv_set_enhanced_suppression(engine, -1) != 0 && mv_set_enhanced_suppression(engine, 5) != 0,
            "Invalid enhanced modes must fail");
    require(mv_set_enhanced_suppression(engine, 1) == 0, "RNNoise must be configurable while stopped");
    mv_destroy(engine);

    for (bool enhanced : {false, true}) for (int level : {-1, 0, 1, 2, 3}) {
        require(pipeline.configure(level, enhanced), "Pipeline initialization failed");
        auto *webRTC = level >= 0 ? mv_ns_create(level) : nullptr;
        auto *rn = enhanced ? rnnoise_create(nullptr) : nullptr;
        require((level < 0 || webRTC) && (!enhanced || rn), "Reference initialization failed");
        std::array<float, 480> block{}, reference{};
        std::mt19937 random(123);
        std::normal_distribution<float> noise(0, 0.02f);
        for (int frame = 0; frame < 120; ++frame) {
            for (size_t i = 0; i < block.size(); ++i) {
                const double t = (frame * 480 + i) / 48000.0;
                block[i] = float(0.15 * std::sin(2 * M_PI * 173 * t) + 0.06 * std::sin(2 * M_PI * 519 * t)) + noise(random);
            }
            reference = block;
            if (webRTC) mv_ns_process(webRTC, reference.data());
            if (rn) {
                for (auto &s : reference) s = std::clamp(s, -1.0f, 32767.0f / 32768.0f) * 32768.0f;
                rnnoise_process_frame(rn, reference.data(), reference.data());
                for (auto &s : reference) s = std::clamp(s / 32768.0f, -1.0f, 1.0f);
            }
            tracking(true);
            pipeline.process(block.data());
            tracking(false);
            for (size_t i = 0; i < block.size(); ++i)
                require(block[i] == reference[i], "Pipeline must match the upstream processors including scale/order/history");
        }
        if (webRTC) mv_ns_destroy(webRTC);
        if (rn) rnnoise_destroy(rn);
        require(pipeline.reset(), "History reset failed");
        for (int frame = 0; frame < 5; ++frame) {
            block.fill(0);
            pipeline.process(block.data());
            for (float s : block) require(s == 0, "Reset must remove all previous audio history");
        }
        // Individual-stage delays at the first/last sample and between them.
        // The nonlinear cascade can move an isolated impulse's largest peak
        // into a filter tail; use a sustained broadband probe for the cascade.
        if (!(enhanced && level >= 0)) for (int offset : {0, 1, 137, 479}) {
            require(pipeline.reset(), "Impulse reset failed");
            std::vector<float> response;
            for (int frame = 0; frame < 7; ++frame) {
                block.fill(0);
                if (frame == 0) block[offset] = 0.5f;
                pipeline.process(block.data());
                response.insert(response.end(), block.begin(), block.end());
            }
            const auto peak = std::max_element(response.begin(), response.end(), [](float a, float b) { return std::abs(a) < std::abs(b); });
            require(std::abs(*peak) > 1e-7, "Impulse must produce nonzero output");
            const auto delay = uint32_t(peak - response.begin() - offset);
            if (delay != mv::NoisePipeline::delayFrames(level, enhanced))
                std::cerr << "Delay mismatch level=" << level << " RNNoise=" << enhanced << " offset=" << offset << " actual=" << delay << '\n';
            require(delay == mv::NoisePipeline::delayFrames(level, enhanced), "Pipeline delay metadata must match the output signal");
        }
        if (enhanced && level >= 0) {
            require(pipeline.reset(), "Correlation reset failed");
            std::vector<float> input, output;
            for (int frame = 0; frame < 150; ++frame) {
                for (auto &sample : block) sample = noise(random) * 5;
                input.insert(input.end(), block.begin(), block.end());
                pipeline.process(block.data());
                output.insert(output.end(), block.begin(), block.end());
            }
            double best = -1;
            uint32_t bestLag = 0;
            for (uint32_t lag = 900; lag < 1700; ++lag) {
                double correlation = 0;
                for (size_t i = 24000; i < 48000; ++i) correlation += double(input[i]) * output[i + lag];
                if (correlation > best) { best = correlation; bestLag = lag; }
            }
            std::cout << "Cascade level " << level << " correlation delay: " << bestLag << " samples\n";
            require(best > 0 && bestLag == mv::NoisePipeline::delayFrames(level, true), "Cascade delay must match broadband signal correlation");
        }
        if (enhanced) {
            require(pipeline.reset(), "Sanitization reset failed");
            block.fill(std::numeric_limits<float>::quiet_NaN());
            block[0] = std::numeric_limits<float>::infinity();
            pipeline.process(block.data());
            for (int frame = 0; frame < 5; ++frame) {
                block.fill(frame == 0 ? 5.0f : 0.0f);
                pipeline.process(block.data());
                for (float s : block) require(std::isfinite(s) && std::abs(s) <= 1, "Malformed input must not poison RNNoise state");
            }
        }
    }
    std::cout << "PASS all pipeline combinations: upstream parity, bypass, reset, bounded output, and exact signal delay\n";

    // Exercise real inference on non-silent input. Silence skips most model work.
    for (int level : {-1, 1}) {
        require(pipeline.configure(level, true), "Inference benchmark initialization failed");
        std::mt19937 random(2026);
        std::normal_distribution<float> noise(0, 0.025f);
        std::array<float, 480> block{};
        std::vector<double> timings;
        double inPower = 0, outPower = 0;
        for (int frame = 0; frame < 1200; ++frame) {
            for (auto &s : block) { s = noise(random); if (frame >= 300) inPower += s * s; }
            const auto start = std::chrono::steady_clock::now();
            tracking(true);
            pipeline.process(block.data());
            tracking(false);
            const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (frame >= 300) timings.push_back(elapsed);
            for (float s : block) {
                require(std::isfinite(s) && std::abs(s) <= 1, "Inference output must be finite and normalized");
                if (frame >= 300) outPower += s * s;
            }
        }
        std::sort(timings.begin(), timings.end());
        const double attenuation = 10 * std::log10(inPower / outPower);
        require(std::isfinite(attenuation) && attenuation > 3, "RNNoise must suppress stationary noise after adaptation");
        std::cout << "PASS " << (level >= 0 ? "WebRTC + RNNoise" : "RNNoise") << ": synthetic noise attenuation " << attenuation
                  << " dB; compute p50/p99/max " << timings[timings.size()/2] << '/' << timings[size_t(timings.size()*0.99)] << '/' << timings.back() << " ms\n";
    }
    pipeline.clear();
}

static void framing() {
    auto queue = std::make_unique<mv::CaptureQueue>();
    std::vector<float> captured(512);
    std::array<float, 480> processed;
    uint64_t produced = 0, consumed = 0;
    for (int callback = 0; callback < 100; ++callback) {
        for (size_t i = 0; i < captured.size(); ++i) captured[i] = float(produced + i);
        require(queue->push(captured.data(), 512, 1e9 + produced * 1e9 / 48000, 48000), "Capture queue overflowed");
        produced += 512;
        double time = 0;
        while (queue->pop(processed.data(), 480, time)) {
            require(std::abs(time - (1e9 + consumed * 1e9 / 48000)) < 0.01, "Framing changed source timestamps");
            for (size_t i = 0; i < processed.size(); ++i) require(processed[i] == consumed + i, "Framing reordered or lost samples");
            consumed += 480;
        }
    }
    require(produced - consumed < 480, "Unexpected frame accumulation");
    require(!queue->push(captured.data(), mv::CaptureQueue::capacity + 1, 0, 48000), "Oversized capture must be rejected");
    require(queue->overruns() == 1, "Capture overrun counter missing");
    queue->discard();
    double time = 0;
    require(!queue->pop(processed.data(), 480, time), "Discard must remove stale audio");
    auto transport = std::make_unique<mv::Transport>();
    transport->configure(48000, 48000, 544);
    transport->markDiscontinuity();
    require(transport->render(processed.data(), 480).resync, "Processor reset must invalidate queued output");
    std::cout << "PASS 512-to-480 framing, timestamps, overflow rejection, discontinuity handoff\n";
}

static void phaseDelays() {
    for (int offset : {0, 1, 2, 137, 479}) {
        auto *ns = mv_ns_create(1);
        std::array<float, 480> block{};
        std::vector<float> output;
        block[offset] = 0.5f;
        for (int frame = 0; frame < 3; ++frame) {
            mv_ns_process(ns, block.data());
            output.insert(output.end(), block.begin(), block.end());
            block.fill(0);
        }
        const auto peak = std::max_element(output.begin(), output.end(), [](float a, float b) { return std::abs(a) < std::abs(b); });
        require(uint32_t(peak - output.begin() - offset) == mv_ns_delay_frames(), "Algorithm delay must hold across frame and filter phases");
        mv_ns_destroy(ns);
    }
    std::cout << "PASS algorithm delay across filter phases and frame boundaries\n";
}

static uint64_t workerPhaseSimulation(uint32_t target) {
    auto queue = std::make_unique<mv::CaptureQueue>();
    auto transport = std::make_unique<mv::Transport>();
    transport->configure(48000, 48000, target);
    std::array<float, 512> capture{}, output{};
    std::array<float, 480> work{};
    uint64_t underruns = 0;
    auto process = [&] {
        double hostNS = 0;
        while (queue->pop(work.data(), 480, hostNS))
            require(transport->push(work.data(), 480, hostNS - mv_ns_delay_frames() * 1e9 / 48000), "Worker output overflowed");
    };
    for (int callback = 0; callback < 3000; ++callback) {
        require(queue->push(capture.data(), 512, 1e9 + callback * 512e9 / 48000, 48000), "Worker input overflowed");
        // Periodically move the worker completion across the render boundary,
        // at different 512/480 frame phases. No source samples are lost.
        const bool late = callback % 17 == 13 || callback % 29 == 7;
        if (!late) process();
        underruns += transport->render(output.data(), 512).underrun;
        if (late) process();
    }
    return underruns;
}
void deepFilterTests();
void deepFilterSpeechTest(const char *path);
int main(int argc, char **argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--speech") {
            deepFilterSpeechTest(argv[2]);
            return 0;
        }
        pipelineTests();
        framing();
        phaseDelays();
        require(workerPhaseSimulation(1056) > 0, "Regression setup must reproduce the old worker scheduling underrun");
        require(workerPhaseSimulation(1536) == 0, "Worker scheduling phase changes must not underflow the protected queue");
        std::cout << "PASS worker/render phase-jitter regression with one processing-hop headroom\n";
        require(!mv_ns_create(-1) && !mv_ns_create(4), "Invalid levels must be rejected");
        for (int level = 0; level < 4; ++level) {
            auto *ns = mv_ns_create(level);
            require(ns, "Noise suppressor allocation failed");
            std::array<float, 480> block{};
            for (int j = 0; j < 20; ++j) {
                block.fill(0);
                tracking(true);
                mv_ns_process(ns, block.data());
                tracking(false);
                for (float s : block) require(s == 0, "Silence must remain silent");
            }
            block[0] = 0.5f;
            std::vector<float> impulse;
            for (int j = 0; j < 3; ++j) {
                tracking(true);
                mv_ns_process(ns, block.data());
                tracking(false);
                impulse.insert(impulse.end(), block.begin(), block.end());
                block.fill(0);
            }
            const auto peak = std::max_element(impulse.begin(), impulse.end(), [](float a, float b) { return std::abs(a) < std::abs(b); });
            std::cout << "Level " << level << " impulse peak: " << (peak - impulse.begin()) << " frames\n";
            require(uint32_t(peak - impulse.begin()) == mv_ns_delay_frames(), "Algorithm delay metadata does not match actual impulse response");
            mv_ns_destroy(ns);
            ns = mv_ns_create(level);
            std::mt19937 random(2026);
            std::normal_distribution<float> noise(0, 0.025f);
            double inPower = 0, outPower = 0;
            const auto start = std::chrono::steady_clock::now();
            for (int frame = 0; frame < 1000; ++frame) {
                for (auto &s : block) { s = noise(random); if (frame > 300) inPower += s * s; }
                tracking(true);
                mv_ns_process(ns, block.data());
                tracking(false);
                for (float s : block) {
                    require(std::isfinite(s) && std::abs(s) <= 1, "Output must stay finite and normalized");
                    if (frame > 300) outPower += s * s;
                }
            }
            const double attenuation = 10 * std::log10(inPower / outPower);
            const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::cout << "PASS level " << level << ": stationary noise reduction " << attenuation
                      << " dB; test loop " << elapsed / 1000 << " ms/block\n";
            require(attenuation > 3, "Stationary noise must be suppressed after adaptation");
            mv_ns_destroy(ns);
        }
        require(allocations == 0, "Processing must not allocate heap memory");
        require(!cAllocationCount || cAllocationCount() == cAllocationBaseline, "Processing must not allocate C heap memory");
        std::cout << "PASS no heap allocation during WebRTC/RNNoise processing (C allocator hooks disabled under ASan)\n";
        deepFilterTests();
        return 0;
    } catch (const std::exception &e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
