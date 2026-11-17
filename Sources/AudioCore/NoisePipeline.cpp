#include "NoisePipeline.hpp"
#include "RoomDereverb.hpp"
#include "mv_webrtc_ns.h"
#include "rnnoise.h"
#include "mv_deepfilter.h"
#include "mv_apple_df.h"
#include "mv_dpdf.h"
#include "mv_localvqe.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

namespace mv {
struct NoisePipeline::State {
    int level;
    int enhanced;
    std::unique_ptr<RoomDereverb> room;
    std::unique_ptr<MVNoiseSuppressor, decltype(&mv_ns_destroy)> webRTC{nullptr, mv_ns_destroy};
    std::unique_ptr<DenoiseState, decltype(&rnnoise_destroy)> rnnoise{nullptr, rnnoise_destroy};
    std::unique_ptr<MVDeepFilter, decltype(&mv_df_destroy)> deepFilter{nullptr, mv_df_destroy};
    std::unique_ptr<MVAppleDF, decltype(&mv_apple_df_destroy)> apple{nullptr, mv_apple_df_destroy};
    // Destroy streaming DSP before the predictor referenced by its context.
    std::unique_ptr<MVDFStream, decltype(&mv_df_stream_destroy)> stream{nullptr, mv_df_stream_destroy};
    std::unique_ptr<MVDPDF, decltype(&mv_dpdf_destroy)> dpdf{nullptr, mv_dpdf_destroy};
    std::unique_ptr<MVLocalVQE, decltype(&mv_localvqe_destroy)> localVQE{nullptr, mv_localvqe_destroy};
    bool coreMLRequested = false;
    std::array<float, frameSize> scaled{};
    State(int level, int enhanced) : level(level), enhanced(enhanced) {}
    bool reset() {
        if (room) room->reset();
        if (level >= 0) {
            webRTC.reset(mv_ns_create(level));
            if (!webRTC) return false;
        }
        if (enhanced == 1) {
            // Upstream rnnoise_create does not check malloc before init.
            // Allocate explicitly so an allocation failure remains recoverable.
            if (!rnnoise) rnnoise.reset(static_cast<DenoiseState *>(std::malloc(rnnoise_get_size())));
            if (!rnnoise || rnnoise_init(rnnoise.get(), nullptr) != 0) return false;
        }
        if (enhanced == 2) {
            if (stream) return mv_apple_df_reset(apple.get()) == 0 && mv_df_stream_reset(stream.get()) == 0;
            if (!deepFilter) deepFilter.reset(mv_df_create(nullptr, 0));
            else if (mv_df_reset(deepFilter.get()) != 0) return false;
            if (!deepFilter || mv_df_delay_frames(deepFilter.get()) != deepFilterDelay) return false;
        }
        if (enhanced == 3 && (!dpdf || mv_dpdf_reset(dpdf.get()) != 0)) return false;
        if (enhanced == 4 && (!localVQE || mv_localvqe_reset(localVQE.get()) != 0)) return false;
        return true;
    }
};
NoisePipeline::NoisePipeline() = default;
NoisePipeline::~NoisePipeline() = default;
bool NoisePipeline::configure(int level, int enhanced, bool preferCoreML, const char *modelPath, int computeUnits, const char *initialStatePath, bool roomDereverb) {
    clear();
    if (level < -1 || level > 3 || enhanced < 0 || enhanced > 4 || rnnoise_get_frame_size() != int(frameSize)) return false;
    if (enhanced == 4) roomDereverb = false;
    if (level < 0 && !enhanced && !roomDereverb) return true;
    try {
        auto next = std::make_unique<State>(level, enhanced);
        if (roomDereverb) next->room = std::make_unique<RoomDereverb>();
        if (enhanced == 3) next->dpdf.reset(mv_dpdf_create(modelPath, initialStatePath, computeUnits, nullptr, 0));
        if (enhanced == 4) next->localVQE.reset(mv_localvqe_create(modelPath, 0, nullptr, 0));
        if (enhanced == 2 && preferCoreML) {
            next->coreMLRequested = true;
            next->apple.reset(mv_apple_df_create(modelPath, computeUnits, nullptr, 0));
            if (next->apple) next->stream.reset(mv_df_stream_create(mv_apple_df_predict, next->apple.get()));
            if (!next->stream) next->apple.reset();
        }
        if (!next->reset()) return false;
        state = std::move(next);
        return true;
    } catch (...) { return false; }
}
void NoisePipeline::clear() { state.reset(); }
bool NoisePipeline::reset() { return !state || state->reset(); }
bool NoisePipeline::process(float *samples) {
    if (!state) return true;
    if (state->room && !state->room->process(samples)) return false;
    if (state->webRTC) mv_ns_process(state->webRTC.get(), samples);
    if (state->rnnoise) {
        // RNNoise uses FloatS16 amplitudes, not Core Audio's normalized floats.
        for (uint32_t i = 0; i < frameSize; ++i) {
            const float sample = std::isfinite(samples[i]) ? samples[i] : 0;
            state->scaled[i] = std::clamp(sample, -1.0f, 32767.0f / 32768.0f) * 32768.0f;
        }
        rnnoise_process_frame(state->rnnoise.get(), state->scaled.data(), state->scaled.data());
        for (uint32_t i = 0; i < frameSize; ++i) {
            const float sample = state->scaled[i] / 32768.0f;
            samples[i] = std::isfinite(sample) ? std::clamp(sample, -1.0f, 1.0f) : 0;
        }
    }
    if (state->deepFilter || state->stream || state->dpdf || state->localVQE) {
        // libDF expects normalized audio and disjoint input/output buffers.
        for (uint32_t i = 0; i < frameSize; ++i)
            state->scaled[i] = std::isfinite(samples[i]) ? std::clamp(samples[i], -1.0f, 1.0f) : 0;
        const auto result = state->localVQE ? mv_localvqe_process(state->localVQE.get(), state->scaled.data(), samples)
            : state->dpdf ? mv_dpdf_process(state->dpdf.get(), state->scaled.data(), samples) : state->stream
            ? mv_df_stream_process(state->stream.get(), state->scaled.data(), samples)
            : mv_df_process(state->deepFilter.get(), state->scaled.data(), samples);
        if (result != 0) {
            std::fill_n(samples, frameSize, 0.0f);
            return false;
        }
        for (uint32_t i = 0; i < frameSize; ++i) samples[i] = std::clamp(samples[i], -1.0f, 1.0f);
    }
    return true;
}
int NoisePipeline::deepFilterBackend() const {
    if (!state || state->enhanced != 2) return 0;
    return state->stream ? 2 : state->coreMLRequested ? 3 : 1;
}
uint32_t NoisePipeline::computeDevices() const {
    return state && state->apple ? mv_apple_df_devices(state->apple.get()) : 0;
}
uint32_t NoisePipeline::dpdfComputeDevices() const {
    return state && state->dpdf ? mv_dpdf_devices(state->dpdf.get()) : 0;
}
uint32_t NoisePipeline::localVQEThreads() const {
    return state ? mv_localvqe_threads(state->localVQE.get()) : 0;
}
uint32_t NoisePipeline::delayFrames(int level, int enhanced, bool roomDereverb) {
    // RNNoise: one analysis/synthesis hop plus one delayed-spectrum hop.
    // Individual impulses and cascade broadband correlation validate alignment.
    return (roomDereverb && enhanced != 4 ? RoomDereverb::delayFrames : 0) + (level >= 0 ? mv_ns_delay_frames() : 0)
        + (enhanced == 1 ? rnnoiseDelay : enhanced == 2 ? deepFilterDelay : enhanced == 3 ? dpdfDelay
            : enhanced == 4 ? mv_localvqe_delay_frames() : 0);
}
}
