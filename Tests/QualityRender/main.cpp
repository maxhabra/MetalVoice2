// Offline renderer for scripts/quality_scores.py. Input and output are mono
// float32 PCM at 48 kHz. Each file starts with fresh model history.
#include "NoisePipeline.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr auto block = mv::NoisePipeline::frameSize;

struct Mode {
    const char *name;
    int webRTC;
    int enhanced;
    const char *resource = nullptr;
};

static void renderFile(mv::NoisePipeline &pipeline, const fs::path &source,
                       const fs::path &destination, size_t delay, std::vector<double> &timings) {
    const auto bytes = fs::file_size(source);
    if (!bytes || bytes % sizeof(float)) throw std::runtime_error("Invalid PCM file: " + source.string());
    const size_t samples = bytes / sizeof(float);
    const size_t padded = ((samples + delay + block - 1) / block) * block;
    std::vector<float> audio(padded, 0);
    std::ifstream input(source, std::ios::binary);
    if (!input.read(reinterpret_cast<char *>(audio.data()), bytes)) throw std::runtime_error("Could not read PCM");
    if (!pipeline.reset()) throw std::runtime_error("Processor reset failed");
    for (size_t i = 0; i < padded; i += block) {
        auto start = Clock::now();
        if (!pipeline.process(audio.data() + i)) throw std::runtime_error("Processing failed");
        if (i + block <= samples)
            timings.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    if (!std::all_of(audio.begin(), audio.end(), [](float x) { return std::isfinite(x); }))
        throw std::runtime_error("Non-finite output");
    std::ofstream output(destination, std::ios::binary);
    if (!output.write(reinterpret_cast<const char *>(audio.data() + delay), bytes))
        throw std::runtime_error("Could not write PCM");
}

int main(int argc, char **argv) {
    try {
        if (argc != 4) throw std::runtime_error("Usage: quality-render INPUT_DIR OUTPUT_DIR APP_RESOURCES");
        const fs::path outputDir = argv[2], resources = fs::absolute(argv[3]);
        std::vector<fs::path> files;
        for (const auto &entry : fs::directory_iterator(argv[1]))
            if (entry.path().extension() == ".f32") files.push_back(entry.path());
        std::sort(files.begin(), files.end());
        if (files.empty()) throw std::runtime_error("No input files");
        const Mode modes[] = {
            {"Noisy", -1, 0},
            {"WebRTC Low", 0, 0},
            {"WebRTC Medium", 1, 0},
            {"WebRTC High", 2, 0},
            {"WebRTC Very high", 3, 0},
            {"RNNoise", -1, 1},
            {"DeepFilterNet3", -1, 2, "DeepFilterNet3.mlmodelc"},
            {"DPDFNet-2", -1, 3, "DPDFNet2.mlmodelc"},
            {"LocalVQE", -1, 4, "localvqe-v1.3-4.8M-f32.gguf"},
        };
        const auto state = resources / "DPDFNet2.state.f32";
        for (const auto &mode : modes) {
            const auto model = mode.resource ? resources / mode.resource : fs::path{};
            mv::NoisePipeline pipeline;
            if (!pipeline.configure(mode.webRTC, mode.enhanced, true, model.c_str(), 0, state.c_str()))
                throw std::runtime_error(std::string("Could not initialize ") + mode.name);
            if (mode.enhanced == 2 && pipeline.deepFilterBackend() != 2)
                throw std::runtime_error("Expected DeepFilterNet3 Core ML backend");
            const auto delay = pipeline.delayFrames(mode.webRTC, mode.enhanced);
            fs::create_directories(outputDir / mode.name);
            std::vector<double> timings;
            for (const auto &file : files)
                renderFile(pipeline, file, outputDir / mode.name / file.filename(), delay, timings);
            if (timings.empty()) throw std::runtime_error("Inputs are shorter than one block");
            std::sort(timings.begin(), timings.end());
            std::cout << mode.name << " p99 " << timings[size_t(std::ceil(0.99 * timings.size())) - 1]
                      << " ms per 10 ms block, max " << timings.back() << " ms" << std::endl;
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
