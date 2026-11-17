#include "NoisePipeline.hpp"
#include "mv_audio.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <random>
#include <stdexcept>
#include <time.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
void require(bool value, const char *error) { if (!value) throw std::runtime_error(error); }
double cpuTime() {
    timespec value{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value);
    return value.tv_sec + value.tv_nsec / 1e9;
}
struct Run {
    std::vector<float> output;
    std::vector<double> times;
    double cpu = 0;
};
Run process(mv::NoisePipeline &pipeline, const std::vector<float> &input) {
    Run run;
    run.output = input;
    run.times.reserve(input.size() / 480);
    const auto cpu = cpuTime();
    for (size_t pos = 0; pos < input.size(); pos += 480) {
        const auto start = Clock::now();
        require(pipeline.process(run.output.data() + pos), "Audio processing failed");
        run.times.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    run.cpu = cpuTime() - cpu;
    for (auto value : run.output) require(std::isfinite(value) && std::abs(value) <= 1, "Output is invalid");
    return run;
}
void report(const char *name, int level, Run run) {
    std::sort(run.times.begin(), run.times.end());
    std::printf("%s WebRTC=%d: compute p50/p99/max %.4f/%.4f/%.4f ms; offline process CPU %.3f%% of one core at real time\n",
        name, level, run.times[run.times.size()/2], run.times[size_t(run.times.size()*.99)], run.times.back(),
        run.cpu / (run.times.size() * .01) * 100);
}
void compare(const std::vector<float> &expected, const std::vector<float> &actual) {
    double power = 0, error = 0, peak = 0;
    require(expected.size() == actual.size(), "Output length changed");
    for (size_t i = 0; i < expected.size(); ++i) {
        double delta = expected[i] - actual[i];
        power += expected[i] * expected[i]; error += delta * delta;
        peak = std::max(peak, std::abs(delta));
    }
    const double relative = std::sqrt(error / std::max(power, 1e-20));
    std::printf("PARITY relative RMS %.8f; peak absolute %.8f\n", relative, peak);
    require(peak < 1e-4 && relative < 1e-3, "Core ML differs from the upstream output");
}
std::vector<float> fixture(const char *path) {
    std::vector<float> voice;
    if (path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        require(bool(file), "Cannot open mono 48 kHz Float32 speech fixture");
        const auto size = file.tellg();
        require(size >= 48000*4 && size % 4 == 0, "Speech fixture format is invalid");
        voice.resize(size_t(size) / 4);
        file.seekg(0); file.read(reinterpret_cast<char *>(voice.data()), size);
        require(bool(file), "Cannot read speech fixture");
    } else {
        // Reproducible non-stationary input for CI when no speech fixture exists.
        voice.resize(48000 * 3);
        for (size_t i = 0; i < voice.size(); ++i) {
            const double t = i / 48000.;
            for (int harmonic = 1; harmonic <= 12; ++harmonic)
                voice[i] += float(.15 / harmonic * std::sin(2*M_PI*harmonic*(170*t+3*std::sin(t))));
            voice[i] *= float(.5+.5*std::sin(2*M_PI*3*t));
        }
    }
    for (auto value : voice) require(std::isfinite(value) && std::abs(value) <= 1, "Speech fixture is not normalized");
    std::vector<float> input(((voice.size() + 479) / 480 + 60) * 480);
    std::mt19937 random(400);
    std::normal_distribution<float> noise(0, .015f);
    for (size_t i = 0; i < voice.size(); ++i)
        input[480*20+i] = std::clamp(voice[i] + noise(random) + float(.02*std::sin(2*M_PI*60*i/48000)), -1.f, 1.f);
    // Silence followed by the same speech checks restart from an idle input.
    input.insert(input.end(), 48000, 0);
    const auto repeat = input;
    input.insert(input.end(), repeat.begin(), repeat.end());
    return input;
}
}

int main(int argc, char **argv) {
    try {
        require(argc >= 2, "Usage: coreml-tests MODEL.mlmodelc [SPEECH.f32]");
        const auto input = fixture(argc > 2 ? argv[2] : nullptr);
        for (int level : {-1, 0, 1, 2, 3}) {
            mv::NoisePipeline reference, accelerated;
            require(reference.configure(level, 2), "Reference initialization failed");
            require(accelerated.configure(level, 2, true, argv[1]), "Accelerated initialization failed");
            require(accelerated.deepFilterBackend() == 2, "Core ML must actually load; fallback is not an acceleration test");
            std::printf("Core ML preferred-device mask: %u\n", accelerated.computeDevices());
            auto native = process(reference, input);
            auto apple = process(accelerated, input);
            compare(native.output, apple.output);
            report("Tract", level, native); report("CoreML", level, apple);
            require(accelerated.reset(), "Accelerated state reset failed");
            auto repeated = process(accelerated, input);
            compare(apple.output, repeated.output);
            std::array<float,480> malformed{};
            malformed.fill(std::numeric_limits<float>::quiet_NaN());
            malformed[1] = std::numeric_limits<float>::infinity();
            require(accelerated.process(malformed.data()), "Malformed input was not sanitized");
            for (auto value : malformed) require(std::isfinite(value), "Malformed input poisoned the model");
            require(accelerated.configure(level, 1) && accelerated.configure(level, 2, true, argv[1]), "Processor switching failed");
            malformed.fill(0);
            require(accelerated.process(malformed.data()), "Switched processor failed");
            for (auto value : malformed) require(value == 0, "Switching leaked old audio");
            require(mv::NoisePipeline::delayFrames(level, 2) == (level >= 0 ? 1774u : 1440u), "Algorithmic delay changed");
        }
        mv::NoisePipeline fallback;
        require(fallback.configure(-1, 2, true, "/nonexistent-metalvoice-model.mlmodelc"), "Native fallback failed");
        require(fallback.deepFilterBackend() == 3 && fallback.computeDevices() == 0, "Fallback must be reported honestly");
        auto *engine = mv_create();
        require(mv_set_deepfilter_acceleration(engine, 0) == 0 && mv_set_deepfilter_acceleration(engine, 1) == 0 && mv_set_deepfilter_acceleration(engine, 2) == -1, "Backend selection validation failed");
        mv_destroy(engine);
        std::puts("PASS Core ML audio parity, every WebRTC level, reset, silence/recovery, switching, malformed input and native fallback");
        return 0;
    } catch (const std::exception &e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
