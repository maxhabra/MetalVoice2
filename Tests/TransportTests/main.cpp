#include "Transport.hpp"
#include <iostream>
#include <vector>
#include <stdexcept>
#include <memory>
#include <numbers>
#include <thread>

static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

static void boundaries() {
    auto t = std::make_unique<mv::Transport>();
    t->configure(48000, 48000, 544);
    std::vector<float> input(1024, 0.25f), output(256, 1);
    auto r = t->render(output.data(), 256);
    require(!r.valid && !r.underrun, "Startup must prime, not count an underrun");
    for (auto value : output) require(value == 0, "Startup must emit silence");
    require(t->push(input.data(), 1024, 1e9), "Initial push failed");
    r = t->render(output.data(), 256);
    require(r.valid && r.sourceHostNS > 1e9, "Must preserve captured timestamps");
    require(std::abs(output.back() - 0.25) < 1e-5, "Resampler must preserve DC gain");
    t->render(output.data(), 256);
    r = t->render(output.data(), 256);
    require(r.underrun && !r.valid, "Missing audio must underflow, not repeat stale samples");
    for (auto value : output) require(value == 0, "Underflow must emit silence");
    require(t->push(input.data(), 1024, 2e9), "Recovery push failed");
    require(t->render(output.data(), 256).valid, "Must recover after reprime");
    require(!t->push(input.data(), mv::Transport::capacity + 1, 0), "Oversized producer write must be rejected");
    require(t->overruns() == 1, "Overrun counter missing");
    require(t->render(output.data(), 256).resync, "Dropped input must trigger bounded resynchronization");
    t->configure(48000, 48000, 544);
    t->push(input.data(), 1024, 0);
    r = t->render(output.data(), 256);
    require(r.valid && r.sourceHostNS == 0, "Missing hardware timestamps must not become fabricated measurements");
}

static void simulation(double inRate, double outRate, double drift, double duration) {
    auto t = std::make_unique<mv::Transport>();
    const uint32_t captureBlock = 128, renderBlock = 256;
    const auto target = uint32_t(std::ceil(std::max(256.0, renderBlock * inRate / outRate * 2))) + mv::Transport::filterLength(inRate, outRate);
    t->configure(inRate, outRate, target);
    const double capturePeriod = captureBlock / (inRate * (1 + drift / 1e6));
    const double renderPeriod = renderBlock / outRate;
    std::vector<float> input(captureBlock), output(renderBlock);
    double nextInput = 0, nextOutput = 0, errorSquared = 0, maxQueue = 0, minQueue = 1e9;
    uint64_t inputIndex = 0, compared = 0, valid = 0;
    double finalPPM = 0;
    while (nextOutput < duration) {
        if (nextInput <= nextOutput) {
            for (uint32_t i = 0; i < captureBlock; ++i)
                input[i] = float(0.5 * std::sin(2 * std::numbers::pi * 1000 * (inputIndex + i) / inRate));
            require(t->push(input.data(), captureBlock, 1e9 + inputIndex * 1e9 / inRate), "Simulation overran");
            inputIndex += captureBlock;
            nextInput += capturePeriod;
        } else {
            const auto info = t->render(output.data(), renderBlock);
            require(!info.underrun && !info.resync, "Steady clocks must not cause discontinuities");
            if (info.valid) {
                ++valid;
                finalPPM = info.correctionPPM;
                if (nextOutput > 1) {
                    maxQueue = std::max(maxQueue, info.queueFrames);
                    minQueue = std::min(minQueue, info.queueFrames);
                    if (drift == 0) {
                        for (uint32_t i = 0; i < renderBlock; ++i) {
                            double time = (info.sourceHostNS - 1e9) / 1e9 + i / outRate * (1 + info.correctionPPM / 1e6);
                            double expected = 0.5 * std::sin(2 * std::numbers::pi * 1000 * time);
                            errorSquared += std::pow(output[i] - expected, 2);
                            ++compared;
                        }
                    }
                }
                for (float value : output) require(std::isfinite(value) && std::abs(value) <= 0.51, "Invalid resampled output");
            }
            nextOutput += renderPeriod;
        }
    }
    require(valid > 100, "Simulation never produced enough audio");
    require(maxQueue < target + inRate * 0.02 && minQueue > 16, "Queue must remain bounded under drift");
    if (compared) require(std::sqrt(errorSquared / compared) < 0.002, "Sample-rate conversion corrupted the reference tone");
    std::cout << "PASS " << inRate << " -> " << outRate << " Hz, drift " << drift
              << " ppm, " << duration << " simulated seconds; queue " << minQueue << "–" << maxQueue
              << ", correction " << finalPPM << " ppm\n";
}

static void downsampling(double rate, double tone, bool passband) {
    auto transport = std::make_unique<mv::Transport>();
    const unsigned inputCount = unsigned(rate / 100);
    std::vector<float> input(inputCount);
    std::array<float, 80> output{};
    transport->configure(rate, 8000, 2 * inputCount + mv::Transport::filterLength(rate, 8000));
    double energy = 0, error = 0;
    unsigned measured = 0;
    for (int hop = 0; hop < 120; ++hop) {
        for (unsigned i = 0; i < inputCount; ++i)
            input[i] = float(.5 * std::sin(2 * std::numbers::pi * tone * (hop * inputCount + i) / rate));
        require(transport->push(input.data(), inputCount, 1e9 + hop * 1e7), "Downsampling overflow");
        const auto result = transport->render(output.data(), output.size());
        if (hop < 10) continue;
        require(result.valid && !result.underrun, "Downsampling underrun");
        for (size_t i = 0; i < output.size(); ++i) {
            const double time = (result.sourceHostNS - 1e9) / 1e9 + i * (1 + result.correctionPPM / 1e6) / 8000;
            energy += output[i] * output[i];
            error += std::pow(output[i] - .5 * std::sin(2 * std::numbers::pi * tone * time), 2);
        }
        measured += output.size();
    }
    const auto db = 10 * std::log10(energy / (.125 * measured));
    if (passband) require(std::abs(db) < .1 && std::sqrt(error / measured) < .003, "Passband gain/timestamp");
    else require(db < -60, "Downsampler failed 60 dB rejection");
    std::cout << rate << " to 8000 Hz, tone " << tone << " Hz, gain " << db << " dB\n";
}

static void concurrentQueue() {
    auto t = std::make_unique<mv::Transport>();
    t->configure(48000, 48000, 544);
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    std::thread producer([&] {
        std::array<float, 128> input;
        input.fill(0.25f);
        uint64_t frame = 0;
        while (!done.load(std::memory_order_acquire)) {
            if (t->queued() > 2048) { std::this_thread::yield(); continue; }
            if (!t->push(input.data(), uint32_t(input.size()), 1e9 + frame * 1e9 / 48000)) failed = true;
            frame += input.size();
        }
    });
    std::array<float, 128> output;
    uint64_t valid = 0;
    double previousHost = 0;
    while (valid < 10000) {
        if (t->queued() < 768) { std::this_thread::yield(); continue; }
        const auto result = t->render(output.data(), uint32_t(output.size()));
        if (result.underrun || result.resync) failed = true;
        if (result.valid) {
            if (result.sourceHostNS <= previousHost) failed = true;
            previousHost = result.sourceHostNS;
            if (valid > 2) for (const auto sample : output)
                if (!std::isfinite(sample) || std::abs(sample - 0.25f) > 1e-5) failed = true;
            ++valid;
        }
    }
    done.store(true, std::memory_order_release);
    producer.join();
    require(!failed, "Concurrent producer/consumer corrupted audio or timestamps");
    std::cout << "PASS concurrent producer/consumer, 1.28 million rendered samples\n";
}

void routeTests();
int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--route-fixture") {
            routeTests();
            return 0;
        }
        boundaries();
        std::cout << "PASS priming, timestamp validity, DC gain, underrun, overflow, recovery\n";
        simulation(48000, 48000, 0, 4);
        simulation(44100, 48000, 0, 4);
        simulation(48000, 44100, 0, 4);
        simulation(48000, 48000, 500, 120);
        simulation(48000, 48000, -500, 120);
        simulation(44100, 48000, 300, 120);
        for (double rate : {48000., 192000.}) {
            for (double tone : {1000., 3200.}) downsampling(rate, tone, true);
            for (double tone : {4500., 5000., 7000.}) downsampling(rate, tone, false);
        }
        concurrentQueue();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
