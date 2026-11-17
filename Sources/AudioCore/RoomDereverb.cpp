#include "RoomDereverb.hpp"
#include <Accelerate/Accelerate.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>
#include <stdexcept>

namespace mv {
namespace {
constexpr size_t size = 960, bins = size / 2 + 1, taps = 12, gap = 3;
constexpr size_t historySize = taps + gap;
constexpr double alpha = 0.999, powerFloor = 1e-6;
using Complex = std::complex<double>;

struct Predictor {
    std::array<Complex, taps * taps> inverse{};
    std::array<Complex, taps> filter{};
    std::array<Complex, historySize> history{};
    double power = 0;
    float gain = 1;
    void reset() {
        inverse.fill({}); filter.fill({}); history.fill({}); power = 0; gain = 1;
        for (size_t i = 0; i < taps; ++i) inverse[i * taps + i] = 1;
    }
    Complex step(Complex y, size_t cursor) {
        std::array<Complex, taps> x{}, product{};
        Complex estimate{};
        for (size_t i = 0; i < taps; ++i) {
            x[i] = history[(cursor + historySize - gap - i) % historySize];
            estimate += std::conj(filter[i]) * x[i];
        }
        const Complex residual = y - estimate;
        power = 0.9 * power + 0.1 * std::norm(y);
        // Frame-online weighted prediction error / RLS. Adapted from NARA-WPE
        // online_wpe_step (MIT); see ThirdParty/NaraWPE for attribution.
        // Silence freezes adaptation rather than inflating the inverse matrix.
        if (power > powerFloor) {
            double denominator = alpha * std::max(power, powerFloor);
            for (size_t i = 0; i < taps; ++i) {
                for (size_t j = 0; j < taps; ++j) product[i] += inverse[i * taps + j] * x[j];
                denominator += (std::conj(x[i]) * product[i]).real();
            }
            if (std::isfinite(denominator) && denominator > powerFloor) {
                bool valid = true;
                for (size_t i = 0; i < taps; ++i) {
                    filter[i] += product[i] / denominator * std::conj(residual);
                    valid &= std::isfinite(std::norm(filter[i])) && std::norm(filter[i]) < 100;
                    // Maintain exact Hermitian symmetry to limit rounding drift.
                    for (size_t j = i; j < taps; ++j) {
                        Complex value = (inverse[i * taps + j] - product[i] * std::conj(product[j]) / denominator) / alpha;
                        if (i == j) {
                            valid &= std::isfinite(value.real()) && value.real() > 0 && value.real() <= 1e4;
                            value = {value.real(), 0};
                        }
                        inverse[i * taps + j] = value;
                        inverse[j * taps + i] = std::conj(value);
                    }
                }
                if (!valid) { reset(); history[cursor] = y; return y; }
            } else { reset(); history[cursor] = y; return y; }
        }
        history[cursor] = y;
        // Conservative, phase-preserving residual mask: no spectral boosts,
        // maximum 8 dB attenuation. Fast opening protects new speech onsets.
        const float target = float(std::clamp(std::abs(residual) / std::max(std::abs(y), 1e-9), 0.4, 1.0));
        gain += (target > gain ? 0.85f : 0.5f) * (target - gain);
        return y * double(gain);
    }
};
}

struct RoomDereverb::State {
    std::array<float, size> window{}, input{}, real{}, imag{}, fftReal{}, fftImag{};
    std::array<float, hop> overlap{};
    std::array<Predictor, bins> predictors{};
    vDSP_DFT_Setup forward = nullptr, inverse = nullptr;
    size_t cursor = 0;
    ~State() {
        if (forward) vDSP_DFT_DestroySetup(forward);
        if (inverse) vDSP_DFT_DestroySetup(inverse);
    }
};
RoomDereverb::RoomDereverb() : state(std::make_unique<State>()) {
    state->forward = vDSP_DFT_zop_CreateSetup(nullptr, size, vDSP_DFT_FORWARD);
    state->inverse = vDSP_DFT_zop_CreateSetup(nullptr, size, vDSP_DFT_INVERSE);
    if (!state->forward || !state->inverse) throw std::runtime_error("Could not initialize room dereverberation FFT");
    for (size_t i = 0; i < size; ++i)
        state->window[i] = float(std::sin(std::numbers::pi * (double(i) + 0.5) / size));
    reset();
}
RoomDereverb::~RoomDereverb() = default;
void RoomDereverb::reset() {
    state->input.fill(0); state->overlap.fill(0); state->cursor = 0;
    for (auto &p : state->predictors) p.reset();
}
bool RoomDereverb::process(float *samples) {
    if (!samples) return false;
    auto &s = *state;
    for (size_t i = 0; i < hop; ++i) if (!std::isfinite(samples[i])) {
        std::fill_n(samples, hop, 0.f); reset(); return false;
    }
    std::copy_n(samples, hop, s.input.data() + hop);
    vDSP_vmul(s.input.data(), 1, s.window.data(), 1, s.real.data(), 1, size);
    s.imag.fill(0);
    vDSP_DFT_Execute(s.forward, s.real.data(), s.imag.data(), s.fftReal.data(), s.fftImag.data());
    for (size_t b = 1; b + 1 < bins; ++b) {
        const auto value = s.predictors[b].step({s.fftReal[b], s.fftImag[b]}, s.cursor);
        s.fftReal[b] = float(value.real()); s.fftImag[b] = float(value.imag());
    }
    s.cursor = (s.cursor + 1) % historySize;
    s.fftImag[0] = s.fftImag[hop] = 0;
    for (size_t i = bins; i < size; ++i) {
        s.fftReal[i] = s.fftReal[size-i]; s.fftImag[i] = -s.fftImag[size-i];
    }
    vDSP_DFT_Execute(s.inverse, s.fftReal.data(), s.fftImag.data(), s.real.data(), s.imag.data());
    for (size_t i = 0; i < hop; ++i) {
        samples[i] = s.real[i] * s.window[i] / size + s.overlap[i];
        s.overlap[i] = s.real[i+hop] * s.window[i+hop] / size;
    }
    std::copy_n(s.input.data() + hop, hop, s.input.data());
    return true;
}
}
