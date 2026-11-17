#pragma once
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace mv {
// Exactly one capture producer and one render consumer. Reset only with both stopped.
class Transport {
public:
    static constexpr uint64_t capacity = 32768;
    static constexpr int taps = 32, phases = 1024, half = taps / 2;
    struct Frame { float sample = 0; double hostNS = 0; };
    struct RenderInfo {
        bool valid = false;
        double sourceHostNS = 0, queueFrames = 0, correctionPPM = 0;
        bool underrun = false, resync = false;
    };
    void configure(double inputRate, double outputRate, uint32_t targetFrames);
    bool push(const float *samples, uint32_t count, double firstHostNS);
    RenderInfo render(float *output, uint32_t count);
    uint64_t queued() const;
    uint64_t overruns() const { return dropped.load(std::memory_order_relaxed); }
    uint32_t target() const { return targetFrames; }
    // Producer announces a discontinuity without touching the consumer cursor.
    void markDiscontinuity() { epoch.fetch_add(1, std::memory_order_release); }

private:
    alignas(64) std::atomic<uint64_t> write{0};
    alignas(64) std::atomic<uint64_t> read{0};
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> epoch{0};
    std::array<Frame, capacity> frames{};
    std::array<std::array<float, taps>, phases> kernel{};
    double inRate = 48000, nominalRatio = 1, cursor = half;
    double filteredError = 0, integral = 0, correction = 0;
    uint32_t targetFrames = 512, fadeRemaining = 128;
    bool primed = false;
    uint64_t observedDrops = 0;
    uint64_t observedEpoch = 0;
};
}
