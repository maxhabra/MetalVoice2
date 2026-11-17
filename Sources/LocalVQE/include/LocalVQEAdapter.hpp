#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace mv {
// Causal 48 kHz <-> 16 kHz conversion and 480 <-> 256 sample framing.
// Owned by the suppression worker. No dynamic allocation in this adapter.
class LocalVQEAdapter {
public:
    static constexpr size_t block = 480, hop = 256, taps = 193;
    static constexpr size_t framingDelay = 672; // (256 - gcd(256,160)) * 3
    static constexpr size_t filterDelay = taps - 1; // two 96-sample FIR delays
    static constexpr size_t modelDelay = 256 * 3; // causal 512/256 overlap-add
    static constexpr size_t delay = framingDelay + filterDelay + modelDelay;
    using ProcessHop = bool (*)(void *, const float *, float *);

    LocalVQEAdapter() {
        // Kaiser-windowed sinc: 7.2 kHz cutoff, approximately 80 dB stopband
        // from 8 kHz. Filtering both ways prevents aliases and spectral images.
        constexpr double pi = 3.14159265358979323846, cutoff = 7200.0 / 48000.0, beta = 8.6;
        const double denominator = bessel(beta);
        double sum = 0;
        for (size_t i = 0; i < taps; ++i) {
            const double x = double(i) - (taps - 1) / 2;
            const double r = x / ((taps - 1) / 2);
            const double sinc = x == 0 ? 2 * cutoff : std::sin(2 * pi * cutoff * x) / (pi * x);
            coefficients[i] = float(sinc * bessel(beta * std::sqrt(std::max(0.0, 1 - r*r))) / denominator);
            sum += coefficients[i];
        }
        for (auto &v : coefficients) v /= float(sum);
        reset();
    }
    void reset() {
        downHistory.fill(0); upHistory.fill(0); inputHop.fill(0); outputHop.fill(0); fifo.fill(0);
        downCursor = upCursor = downPhase = inputCount = readCursor = 0;
        writeCursor = queued = framingDelay;
    }
    // Input/output may alias. The callback receives exactly 256 normalized
    // 16 kHz samples. It must preserve its temporal state between calls.
    bool process(const float *input, float *output, ProcessHop processHop, void *context) {
        for (size_t i = 0; i < block; ++i) {
            downHistory[downCursor] = downHistory[downCursor + taps] = input[i];
            if (downPhase == 0) {
                float sample = 0;
                const float *history = downHistory.data() + downCursor + taps;
                for (size_t j = 0; j < taps; ++j) sample += coefficients[j] * history[-ptrdiff_t(j)];
                inputHop[inputCount++] = sample;
                if (inputCount == hop) {
                    if (!processHop(context, inputHop.data(), outputHop.data())) return false;
                    if (queued + hop*3 > fifo.size()) return false;
                    for (float value : outputHop) {
                        if (!std::isfinite(value)) return false;
                        upHistory[upCursor] = upHistory[upCursor + upTaps] = value;
                        const float *up = upHistory.data() + upCursor + upTaps;
                        for (size_t phase = 0; phase < 3; ++phase) {
                            float interpolated = 0;
                            for (size_t j = phase, k = 0; j < taps; j += 3, ++k)
                                interpolated += coefficients[j] * up[-ptrdiff_t(k)];
                            fifo[writeCursor] = 3 * interpolated;
                            writeCursor = (writeCursor + 1) % fifo.size();
                        }
                        upCursor = (upCursor + 1) % upTaps;
                    }
                    queued += hop*3;
                    inputCount = 0;
                }
            }
            downCursor = (downCursor + 1) % taps;
            downPhase = (downPhase + 1) % 3;
        }
        if (queued < block) return false;
        for (size_t i = 0; i < block; ++i) {
            output[i] = fifo[readCursor];
            readCursor = (readCursor + 1) % fifo.size();
        }
        queued -= block;
        return true;
    }
private:
    static constexpr size_t upTaps = (taps + 2) / 3;
    static double bessel(double x) {
        double sum = 1, term = 1;
        for (int i = 1; i < 40; ++i) { term *= (x*x/4)/(i*i); sum += term; }
        return sum;
    }
    std::array<float, taps> coefficients{};
    std::array<float, taps*2> downHistory{};
    std::array<float, upTaps*2> upHistory{};
    std::array<float, hop> inputHop{}, outputHop{};
    std::array<float, 1536> fifo{};
    size_t downCursor = 0, upCursor = 0, downPhase = 0, inputCount = 0;
    size_t readCursor = 0, writeCursor = 0, queued = 0;
};
}
