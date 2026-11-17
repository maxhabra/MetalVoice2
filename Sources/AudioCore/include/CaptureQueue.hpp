#pragma once
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace mv {
// Raw capture producer -> noise-processing consumer. No rate conversion here.
// Reset only after both threads stop. The consumer alone owns discard/pop.
class CaptureQueue {
public:
    static constexpr uint64_t capacity = 16384;
    struct Frame { float sample = 0; double hostNS = 0; };
    void reset() { write = 0; read = 0; dropped = 0; }
    bool push(const float *samples, uint32_t count, double firstHostNS, double rate) {
        const auto w = write.load(std::memory_order_relaxed);
        const auto r = read.load(std::memory_order_acquire);
        if (count > capacity || w - r + count > capacity) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        for (uint32_t i = 0; i < count; ++i)
            data[(w + i) % capacity] = {samples[i], firstHostNS > 0 ? firstHostNS + i * 1e9 / rate : 0};
        write.store(w + count, std::memory_order_release);
        return true;
    }
    bool pop(float *samples, uint32_t count, double &hostNS) {
        const auto r = read.load(std::memory_order_relaxed);
        const auto w = write.load(std::memory_order_acquire);
        if (count > w - r) return false;
        hostNS = data[r % capacity].hostNS;
        for (uint32_t i = 0; i < count; ++i) samples[i] = data[(r + i) % capacity].sample;
        read.store(r + count, std::memory_order_release);
        return true;
    }
    void discard() { read.store(write.load(std::memory_order_acquire), std::memory_order_release); }
    uint64_t overruns() const { return dropped.load(std::memory_order_relaxed); }
private:
    alignas(64) std::atomic<uint64_t> write{0};
    alignas(64) std::atomic<uint64_t> read{0};
    std::atomic<uint64_t> dropped{0};
    std::array<Frame, capacity> data{};
};
}
