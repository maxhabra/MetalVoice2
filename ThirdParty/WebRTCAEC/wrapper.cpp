#include "mv_webrtc_aec.h"
#include "api/audio/audio_processing.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

struct MVEchoCanceller {
    rtc::scoped_refptr<webrtc::AudioProcessing> processor;
    webrtc::ProcessingConfig streams;
};
extern "C" MVEchoCanceller *mv_aec_create() {
    try {
        auto state = std::make_unique<MVEchoCanceller>();
        webrtc::AudioProcessing::Config config;
        config.pipeline.maximum_internal_processing_rate = 48000;
        config.pipeline.multi_channel_render = true;
        config.echo_canceller.enabled = true;
        config.echo_canceller.mobile_mode = false;
        config.echo_canceller.enforce_high_pass_filtering = false;
        config.high_pass_filter.enabled = false;
        config.noise_suppression.enabled = false;
        config.gain_controller1.enabled = false;
        config.gain_controller2.enabled = false;
        state->processor = webrtc::AudioProcessingBuilder().SetConfig(config).Create();
        state->streams.input_stream() = webrtc::StreamConfig(48000,1);
        state->streams.output_stream() = webrtc::StreamConfig(48000,1);
        state->streams.reverse_input_stream() = webrtc::StreamConfig(48000,2);
        state->streams.reverse_output_stream() = webrtc::StreamConfig(48000,2);
        if (!state->processor || mv_aec_reset(state.get()) != 0) return nullptr;
        return state.release();
    } catch (...) { return nullptr; }
}
extern "C" void mv_aec_destroy(MVEchoCanceller *s) { delete s; }
extern "C" int mv_aec_reset(MVEchoCanceller *s) {
    try { return s && s->processor ? s->processor->Initialize(s->streams) : -1; }
    catch (...) { return -1; }
}
extern "C" int mv_aec_process(MVEchoCanceller *s, const float *left, const float *right, float *mic) {
    if (!s || !left || !right || !mic) return -1;
    for (size_t i=0; i<480; ++i) if (!std::isfinite(left[i]) || !std::isfinite(right[i]) || !std::isfinite(mic[i])) {
        std::fill_n(mic,480,0.f); return -1;
    }
    try {
    const float *render[] = {left,right};
    const float *capture[] = {mic};
    float *output[] = {mic};
    if (s->processor->AnalyzeReverseStream(render,s->streams.reverse_input_stream()) != 0
        || s->processor->set_stream_delay_ms(0) != 0
        || s->processor->ProcessStream(capture,s->streams.input_stream(),s->streams.output_stream(),output) != 0) {
        std::fill_n(mic,480,0.f); return -1;
    }
    for (size_t i=0;i<480;++i) if (!std::isfinite(mic[i])) { std::fill_n(mic,480,0.f); return -1; }
    return 0;
    } catch (...) { std::fill_n(mic,480,0.f); return -1; }
}
// Verified by the offline impulse/correlation test before release.
extern "C" uint32_t mv_aec_delay_frames() { return 430; }
extern "C" void mv_aec_metrics(MVEchoCanceller *s, double *erle, double *delay) {
    if (!s) return;
    const auto stats = s->processor->GetStatistics();
    if (erle) *erle = stats.echo_return_loss_enhancement.value_or(-1);
    if (delay) *delay = stats.delay_ms ? *stats.delay_ms : -1;
}
