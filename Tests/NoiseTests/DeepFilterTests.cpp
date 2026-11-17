#include "NoisePipeline.hpp"
#include "mv_deepfilter.h"
#include "mv_webrtc_ns.h"
#include "mv_audio.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <fstream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
using DF = std::unique_ptr<MVDeepFilter, decltype(&mv_df_destroy)>;
}

void deepFilterTests() {
    auto *engine = mv_create();
    require(mv_set_noise_suppression(engine, 1) == 0 && mv_set_enhanced_suppression(engine, 2) == 0, "DFN3 engine configuration failed");
    MVStats stats{};
    mv_stats(engine, &stats);
    require(stats.enhanced_suppression == 2 && stats.rnnoise_delay_ms == 0 && stats.deepfilter_delay_ms == 30,
            "Selected backend/delay must be reported correctly");
    require(std::abs(stats.suppression_delay_ms - 1774.0/48) < 1e-9, "Combined delay report is incorrect");
    mv_destroy(engine);
    // The upstream Tract runtime allocates temporary tensors on this worker
    // path. Do not include it in the allocation-free WebRTC/RNNoise assertion.
    DF reference(mv_df_create(nullptr, 0), mv_df_destroy);
    require(bool(reference), "DeepFilterNet3 initialization failed");
    require(mv_df_delay_frames(reference.get()) == mv::NoisePipeline::deepFilterDelay, "Unexpected DFN3 model delay");
    std::array<float, 480> input{}, output{}, expected{}, filtered{};
    for (int level : {-1, 0, 1, 2, 3}) {
        mv::NoisePipeline pipeline;
        require(pipeline.configure(level, 2), "DFN3 pipeline initialization failed");
        require(mv_df_reset(reference.get()) == 0, "Reference reset failed");
        std::unique_ptr<MVNoiseSuppressor, decltype(&mv_ns_destroy)> webRTC(level >= 0 ? mv_ns_create(level) : nullptr, mv_ns_destroy);
        require(level < 0 || bool(webRTC), "WebRTC reference initialization failed");
        std::mt19937 random(2026);
        std::normal_distribution<float> noise(0, 0.03f);
        for (int frame = 0; frame < 120; ++frame) {
            for (size_t i = 0; i < input.size(); ++i) {
                const double t = (frame * 480 + i) / 48000.0;
                input[i] = float(0.12 * std::sin(2 * M_PI * 173 * t) + 0.05 * std::sin(2 * M_PI * 519 * t)) + noise(random);
            }
            output = input;
            filtered = input;
            if (webRTC) mv_ns_process(webRTC.get(), filtered.data());
            require(mv_df_process(reference.get(), filtered.data(), expected.data()) == 0, "Reference inference failed");
            require(pipeline.process(output.data()), "Pipeline inference failed");
            for (size_t i = 0; i < output.size(); ++i)
                require(output[i] == std::clamp(expected[i], -1.0f, 1.0f), "DFN3 pipeline differs from direct native inference");
        }
        // A reset must discard speech and neural/filter history, including
        // after switching from another enhanced processor.
        require(pipeline.reset(), "DFN3 reset failed");
        for (int frame = 0; frame < 8; ++frame) {
            output.fill(0);
            require(pipeline.process(output.data()), "Silence inference failed");
            for (float value : output) require(value == 0, "DFN3 reset leaked old audio");
        }
        output.fill(std::numeric_limits<float>::quiet_NaN());
        output[0] = std::numeric_limits<float>::infinity();
        require(pipeline.process(output.data()), "Pipeline failed to sanitize malformed input");
        for (float value : output) require(std::isfinite(value) && std::abs(value) <= 1, "Malformed input poisoned DFN3 output");
        require(pipeline.configure(level, 1) && pipeline.configure(level, 2), "Enhanced processor switching failed");
        output.fill(0);
        require(pipeline.process(output.data()), "DFN3 failed after switching back from RNNoise");
        for (float value : output) require(value == 0, "Switching must clear old processor state");
    }
    std::cout << "PASS DeepFilterNet3 alone / all WebRTC levels: upstream parity, state reset, switching, malformed input\n";

    for (int level : {-1, 1}) {
        mv::NoisePipeline pipeline;
        require(pipeline.configure(level, 2), "DFN3 benchmark initialization failed");
        std::mt19937 random(17);
        std::normal_distribution<float> noise(0, 0.04f);
        std::vector<double> timings;
        double inPower = 0, outPower = 0;
        for (int frame = 0; frame < 1000; ++frame) {
            for (size_t i = 0; i < input.size(); ++i) {
                const double t = (frame * 480 + i) / 48000.0;
                input[i] = noise(random) + float(0.1 * std::sin(2 * M_PI * 173 * t));
                if (frame >= 200) inPower += input[i] * input[i];
            }
            const auto before = std::chrono::steady_clock::now();
            require(pipeline.process(input.data()), "DFN3 sustained inference failed");
            const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count();
            if (frame >= 200) timings.push_back(ms);
            for (float value : input) {
                require(std::isfinite(value) && std::abs(value) <= 1, "DFN3 sustained output invalid");
                if (frame >= 200) outPower += value * value;
            }
        }
        // A speech denoiser may legitimately remove this stationary test tone.
        // Voice preservation is checked separately using an actual speech fixture.
        require(std::isfinite(outPower) && inPower > 0, "DFN3 energy counters invalid");
        std::sort(timings.begin(), timings.end());
        std::cout << "PASS " << (level >= 0 ? "WebRTC + DeepFilterNet3" : "DeepFilterNet3")
                  << ": non-silent synthetic inference compute p50/p99/max " << timings[timings.size()/2]
                  << '/' << timings[size_t(timings.size() * .99)] << '/' << timings.back() << " ms\n";
    }
}

void deepFilterSpeechTest(const char *path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    require(bool(file), "Cannot open speech fixture");
    const auto bytes = file.tellg();
    require(bytes >= 48000 * 4 && bytes % 4 == 0, "Speech fixture must be mono 48 kHz Float32 PCM, at least one second");
    std::vector<float> clean(size_t(bytes) / sizeof(float));
    file.seekg(0); file.read(reinterpret_cast<char *>(clean.data()), bytes);
    require(bool(file), "Cannot read speech fixture");
    for (float sample : clean) require(std::isfinite(sample) && std::abs(sample) <= 1, "Speech fixture must be normalized Float32");
    for (int level : {-1, 1}) {
        mv::NoisePipeline pipeline;
        require(pipeline.configure(level, 2), "DFN3 speech initialization failed");
        std::mt19937 random(400);
        std::normal_distribution<float> noise(0, .015f);
        std::vector<float> output;
        std::vector<double> times;
        std::array<float, 480> block{};
        const auto delay = mv::NoisePipeline::delayFrames(level, 2);
        const auto total = ((clean.size() + delay + 479) / 480) * 480;
        for (size_t pos = 0; pos < total; pos += 480) {
            for (size_t i = 0; i < 480; ++i)
                block[i] = pos + i < clean.size() ? std::clamp(clean[pos+i] + noise(random) + float(.02 * std::sin(2 * M_PI * 60 * (pos+i) / 48000)), -1.f, 1.f) : 0;
            const auto start = std::chrono::steady_clock::now();
            require(pipeline.process(block.data()), "DFN3 speech inference failed");
            times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
            output.insert(output.end(), block.begin(), block.end());
        }
        double inputEnergy = 0, outputEnergy = 0, dot = 0;
        for (size_t i = 0; i < clean.size(); ++i) {
            inputEnergy += clean[i] * clean[i];
            outputEnergy += output[i+delay] * output[i+delay];
            dot += clean[i] * output[i+delay];
        }
        const double correlation = dot / std::sqrt(inputEnergy * outputEnergy);
        require(std::isfinite(correlation) && correlation > 0.5, "DFN3 did not preserve the speech fixture's signal");
        std::sort(times.begin(),times.end());
        std::cout << "PASS " << (level >= 0 ? "WebRTC + DFN3" : "DFN3")
                  << " noisy speech: aligned correlation " << correlation << "; compute p50/p99/max "
                  << times[times.size()/2] << '/' << times[size_t(times.size()*.99)] << '/' << times.back() << " ms\n";
    }
}
