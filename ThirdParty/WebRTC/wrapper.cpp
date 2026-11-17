#include "mv_webrtc_ns.h"
#include "modules/audio_processing/audio_buffer.h"
#include "modules/audio_processing/ns/noise_suppressor.h"
#include "modules/audio_processing/three_band_filter_bank.h"
#include <algorithm>
#include <cmath>

struct MVNoiseSuppressor {
    webrtc::AudioBuffer buffer;
    webrtc::ThreeBandFilterBank filter;
    webrtc::NoiseSuppressor suppressor;
    std::array<float, 480> full{};
    explicit MVNoiseSuppressor(int level)
        : suppressor(webrtc::NsConfig{static_cast<webrtc::NsConfig::SuppressionLevel>(level)}, 48000, 1) {}
};

extern "C" MVNoiseSuppressor *mv_ns_create(int level) {
    if (level < 0 || level > 3) return nullptr;
    try { return new MVNoiseSuppressor(level); } catch (...) { return nullptr; }
}
extern "C" void mv_ns_destroy(MVNoiseSuppressor *ns) { delete ns; }
extern "C" void mv_ns_process(MVNoiseSuppressor *ns, float *samples) {
    for (size_t i = 0; i < 480; ++i) {
        const float sample = std::isfinite(samples[i]) ? samples[i] : 0;
        ns->full[i] = std::clamp(sample * 32768.0f, -32768.0f, 32767.0f);
    }
    std::array<webrtc::ArrayView<float>, 3> bands{
        ns->buffer.bands[0], ns->buffer.bands[1], ns->buffer.bands[2]};
    ns->filter.Analysis(ns->full, bands);
    ns->suppressor.Analyze(ns->buffer);
    ns->suppressor.Process(&ns->buffer);
    ns->filter.Synthesis(bands, ns->full);
    for (size_t i = 0; i < 480; ++i) samples[i] = std::clamp(ns->full[i] / 32768.0f, -1.0f, 1.0f);
}
extern "C" uint32_t mv_ns_delay_frames() {
    // 96 samples of 16 kHz NS overlap + analysis/synthesis filter-bank delay.
    // Checked against the full-band impulse peak by noise-tests.
    return 288 + 46;
}
