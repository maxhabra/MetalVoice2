#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace mv {
// One capture producer, one processor. Reset only while both are stopped.
class CaptureQueue {
public:
    static constexpr uint64_t capacity = 16384;
    enum class Read { waiting, gap, ready };

    void reset(double rate = 48000) {
        samplePeriod = 1e9 / rate;
        write = 0; read = 0; dropped = 0;
        expectedNS = 0; segment = 0; haveInput = false; lostInput = false;
        current = {}; spanOffset = 0; consumedSegment = 0; afterDiscard = false;
    }
    bool push(const float *samples, uint32_t count, double hostNS) {
        if (!count) return true;
        const auto w = write.load(std::memory_order_relaxed);
        const auto r = read.load(std::memory_order_acquire);
        if (count > capacity || w - r + count > capacity) {
            lostInput = true;
            dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const bool timed = hostNS > 0 && std::isfinite(hostNS);
        if (lostInput || (haveInput && (timed != (expectedNS > 0)
                || (timed && std::abs(hostNS - expectedNS) > 2 * samplePeriod)))) ++segment;
        expectedNS = timed ? hostNS + count * samplePeriod : 0;
        haveInput = true;
        lostInput = false;
        const size_t offset = w % capacity;
        spans[offset] = {timed ? hostNS : 0, count, segment};
        const size_t first = std::min<size_t>(count, capacity - offset);
        std::copy_n(samples, first, pcm.data() + offset);
        std::copy_n(samples + first, count - first, pcm.data());
        write.store(w + count, std::memory_order_release);
        return true;
    }

    Read pop(float *samples, uint32_t count, double &hostNS) {
        auto r = read.load(std::memory_order_relaxed);
        if (!count || write.load(std::memory_order_acquire) - r < count) return Read::waiting;
        uint32_t copied = 0;
        while (copied < count) {
            if (!spanOffset) current = spans[r % capacity];
            if (afterDiscard) {
                // The worker already reset its filters when it discarded the queue.
                consumedSegment = current.segment;
                afterDiscard = false;
            }
            if (current.segment != consumedSegment) {
                // Discard only the incomplete pre-gap hop. Leave the next span intact.
                consumedSegment = current.segment;
                read.store(r, std::memory_order_release);
                return Read::gap;
            }
            if (!copied) hostNS = current.hostNS > 0 ? current.hostNS + spanOffset * samplePeriod : 0;
            const size_t offset = r % capacity;
            const auto n = std::min({count - copied, current.count - spanOffset, uint32_t(capacity - offset)});
            std::copy_n(pcm.data() + offset, n, samples + copied);
            copied += n;
            r += n;
            spanOffset += n;
            if (spanOffset == current.count) spanOffset = 0;
        }
        read.store(r, std::memory_order_release);
        return Read::ready;
    }
    void discard() {
        afterDiscard = true;
        spanOffset = 0;
        read.store(write.load(std::memory_order_acquire), std::memory_order_release);
    }
    uint64_t overruns() const { return dropped.load(std::memory_order_relaxed); }
private:
    struct Span { double hostNS; uint32_t count, segment; };
    double samplePeriod = 1e9 / 48000;
    alignas(64) std::atomic<uint64_t> write{0};
    double expectedNS = 0;
    uint32_t segment = 0;
    bool haveInput = false, lostInput = false;
    alignas(64) std::atomic<uint64_t> read{0};
    bool afterDiscard = false;
    Span current{};
    uint32_t spanOffset = 0, consumedSegment = 0;
    std::atomic<uint64_t> dropped{0};
    // The span at a callback's first sample is published with its PCM. The
    // consumer keeps its own copy when a read ends inside that callback.
    std::array<float, capacity> pcm{};
    std::array<Span, capacity> spans{};
};
}
