#include "Transport.hpp"
#include <cstring>
#include <numbers>

namespace mv {
void Transport::configure(double inputRate, double outputRate, uint32_t target) {
    inRate = inputRate;
    nominalRatio = inputRate / outputRate;
    targetFrames = std::clamp(target, uint32_t(taps * 2), uint32_t(capacity / 4));
    write.store(0); read.store(0); dropped.store(0);
    epoch.store(0); observedEpoch = 0;
    cursor = half; primed = false; fadeRemaining = 128; observedDrops = 0;
    filteredError = integral = correction = 0;
    // Polyphase, Blackman-windowed sinc. Built once, away from audio callbacks.
    const double cutoff = 0.47 * std::min(1.0, outputRate / inputRate);
    for (int p = 0; p < phases; ++p) {
        const double frac = double(p) / phases;
        double sum = 0;
        for (int j = 0; j < taps; ++j) {
            const double x = j - (half - 1) - frac;
            const double a = 2 * std::numbers::pi * cutoff * x;
            const double sinc = std::abs(a) < 1e-10 ? 1 : std::sin(a) / a;
            const double window = 0.42 - 0.5 * std::cos(2 * std::numbers::pi * j / (taps - 1))
                                + 0.08 * std::cos(4 * std::numbers::pi * j / (taps - 1));
            kernel[p][j] = float(2 * cutoff * sinc * window);
            sum += kernel[p][j];
        }
        for (auto &v : kernel[p]) v = float(v / sum);
    }
}

bool Transport::push(const float *samples, uint32_t count, double firstHostNS) {
    const auto w = write.load(std::memory_order_relaxed);
    const auto r = read.load(std::memory_order_acquire);
    if (count > capacity || w - r + count > capacity) {
        dropped.fetch_add(1, std::memory_order_relaxed);
        return false; // Producer never moves the consumer cursor or overwrites unread data.
    }
    const double step = 1e9 / inRate;
    for (uint32_t i = 0; i < count; ++i)
        frames[(w + i) % capacity] = {samples[i], firstHostNS > 0 ? firstHostNS + i * step : 0};
    write.store(w + count, std::memory_order_release);
    return true;
}

uint64_t Transport::queued() const {
    // Snapshot can race with the other cursor; clamp instead of unsigned underflow.
    const auto r = read.load(std::memory_order_acquire);
    const auto w = write.load(std::memory_order_acquire);
    return w >= r ? std::min(w - r, capacity) : 0;
}

Transport::RenderInfo Transport::render(float *output, uint32_t count) {
    RenderInfo info;
    std::fill_n(output, count, 0.0f);
    const auto w = write.load(std::memory_order_acquire);
    const auto r = read.load(std::memory_order_relaxed);
    const auto drops = dropped.load(std::memory_order_acquire);
    const auto currentEpoch = epoch.load(std::memory_order_acquire);
    if (drops != observedDrops || currentEpoch != observedEpoch || (primed && double(w) - cursor > targetFrames + inRate * 0.1)) {
        observedDrops = drops;
        observedEpoch = currentEpoch;
        primed = false;
        read.store(w, std::memory_order_release);
        info.resync = true;
        return info;
    }
    if (!primed) {
        if (w - r < targetFrames + half) return info;
        cursor = double(w - targetFrames);
        primed = true;
        fadeRemaining = 128;
        filteredError = integral = correction = 0;
    }
    // Slow PI occupancy control handles independent nominally equal hardware clocks.
    // A moving average rejects callback-sized queue oscillations. Correction <= 2000 ppm.
    const double dt = count * nominalRatio / inRate;
    const double error = (double(w) - cursor - targetFrames) / inRate;
    filteredError += std::min(1.0, dt / 0.5) * (error - filteredError);
    integral = std::clamp(integral + filteredError * dt * 0.015, -0.001, 0.001);
    correction = std::clamp(filteredError * 0.25 + integral, -0.002, 0.002);
    const double ratio = nominalRatio * (1 + correction);
    info.queueFrames = double(w) - cursor;
    info.correctionPPM = correction * 1e6;
    if (!count) return info;
    const auto lastNeeded = uint64_t(std::floor(cursor + (count - 1) * ratio)) + half;
    if (lastNeeded >= w) {
        info.underrun = true;
        primed = false;
        read.store(w, std::memory_order_release);
        return info;
    }
    const auto center = uint64_t(cursor);
    const auto &a = frames[center % capacity];
    const auto &b = frames[(center + 1) % capacity];
    info.sourceHostNS = a.hostNS > 0 && b.hostNS > 0
        ? a.hostNS + (cursor - center) * (b.hostNS - a.hostNS) : 0;
    info.valid = true;
    for (uint32_t i = 0; i < count; ++i) {
        const auto c = uint64_t(cursor);
        const double frac = cursor - c;
        const auto phase = std::min(phases - 1, int(frac * phases));
        double value = 0;
        for (int j = 0; j < taps; ++j)
            value += frames[(c - (half - 1) + j) % capacity].sample * kernel[phase][j];
        if (fadeRemaining) { value *= (128 - fadeRemaining) / 128.0; --fadeRemaining; }
        output[i] = float(value);
        cursor += ratio;
    }
    // Retain the left filter wing until the consumer has finished reading it.
    read.store(uint64_t(cursor) - (half - 1), std::memory_order_release);
    return info;
}
}
