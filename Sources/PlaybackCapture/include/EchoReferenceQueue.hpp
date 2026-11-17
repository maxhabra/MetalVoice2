#pragma once
#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace mv {
// Tap producer -> microphone worker. Host-time interpolation aligns both
// clocks without discarding channels, growing a queue or blocking a callback.
class EchoReferenceQueue {
public:
    static constexpr uint64_t capacity = 65536;
    enum Result { ready = 1, waiting = 0, missing = -1 };
    struct Frame { float left, right; double hostNS; };
    bool push(const float *left, const float *right, uint32_t count, uint32_t stride,
              double hostNS, double rate) {
        const auto w = write.load(std::memory_order_relaxed);
        const auto r = read.load(std::memory_order_acquire);
        if (!left || !right || !count || !stride || !std::isfinite(hostNS) || hostNS <= 0
            || !std::isfinite(rate) || rate < 32000 || rate > 192000 || (lastHostNS > 0 && hostNS <= lastHostNS) || count > capacity || w-r+count > capacity) {
            dropped.fetch_add(1,std::memory_order_relaxed); return false;
        }
        const double step = 1e9/rate;
        // Reject the entire malformed block, preserving a detectable time gap.
        for (uint32_t i=0;i<count;++i)
            if (!std::isfinite(left[i*stride]) || !std::isfinite(right[i*stride])) {
                dropped.fetch_add(1,std::memory_order_relaxed); return false;
            }
        for (uint32_t i=0;i<count;++i)
            frames[(w+i)%capacity] = {left[i*stride],right[i*stride],hostNS+i*step};
        lastHostNS = hostNS + (count-1)*step;
        write.store(w+count,std::memory_order_release); return true;
    }
    Result readBlock(double firstHostNS, float *left, float *right, uint32_t count = 480) {
        if (!left || !right || !std::isfinite(firstHostNS) || firstHostNS <= 0 || !count || count > 8192) return missing;
        auto r = read.load(std::memory_order_relaxed);
        const auto w = write.load(std::memory_order_acquire);
        if (w-r < 2) return waiting;
        constexpr double step = 1e9/48000.;
        const double end = firstHostNS+(count-1)*step;
        if (firstHostNS < frames[r%capacity].hostNS - step*.25) return missing;
        // Keep the final predecessor even while waiting, freeing old storage.
        while (r+1 < w && frames[(r+1)%capacity].hostNS <= firstHostNS) ++r;
        read.store(r,std::memory_order_release);
        if (end > frames[(w-1)%capacity].hostNS + .5) return waiting;
        for (uint32_t i=0;i<count;++i) {
            const double target = firstHostNS+i*step;
            while (r+1 < w && frames[(r+1)%capacity].hostNS <= target) ++r;
            const auto a=frames[r%capacity];
            const auto b=frames[(r+1 < w ? r+1 : r)%capacity];
            const double span=b.hostNS-a.hostNS;
            if (span < 0 || span > step*2.5 || target < a.hostNS-step*.25) return missing;
            const double mix=span>0 ? std::clamp((target-a.hostNS)/span,0.,1.) : 0;
            left[i]=float(a.left+(b.left-a.left)*mix);
            right[i]=float(a.right+(b.right-a.right)*mix);
        }
        read.store(r,std::memory_order_release); return ready;
    }
    void discard() { read.store(write.load(std::memory_order_acquire),std::memory_order_release); }
    uint64_t overruns() const { return dropped.load(std::memory_order_relaxed); }
private:
    double lastHostNS = 0; // Producer-only; timestamps must remain monotonic.
    alignas(64) std::atomic<uint64_t> write{0}, read{0};
    std::atomic<uint64_t> dropped{0};
    std::array<Frame,capacity> frames{};
};
}
