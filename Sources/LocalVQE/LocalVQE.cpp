#include "mv_localvqe.h"
#include "LocalVQEAdapter.hpp"
#include "localvqe_api.h"
#include <CoreFoundation/CoreFoundation.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>

struct MVLocalVQE {
    localvqe_ctx_t context = 0;
    uint32_t threads = 2;
    mv::LocalVQEAdapter adapter;
    ~MVLocalVQE() { localvqe_free(context); }
    static bool processHop(void *opaque, const float *input, float *output) {
        auto *self = static_cast<MVLocalVQE *>(opaque);
        static constexpr std::array<float, 256> silentReference{};
        return localvqe_process_frame_f32(self->context, input, silentReference.data(), 256, output) == 0;
    }
};
extern "C" MVLocalVQE *mv_localvqe_create(const char *path, int threads, char *error, size_t capacity) {
    try {
        if (threads < 0 || threads > 4) throw std::runtime_error("Invalid LocalVQE thread count.");
        std::array<char, 4096> resource{};
        if (!path) {
            CFBundleRef bundle = CFBundleGetMainBundle();
            CFURLRef url = bundle ? CFBundleCopyResourceURL(bundle, CFSTR("localvqe-v1.3-4.8M-f32"), CFSTR("gguf"), nullptr) : nullptr;
            const bool found = url && CFURLGetFileSystemRepresentation(url, true, reinterpret_cast<UInt8 *>(resource.data()), resource.size());
            if (url) CFRelease(url);
            if (!found) throw std::runtime_error("Bundled LocalVQE model is missing. Rebuild the app.");
            path = resource.data();
        }
        auto state = std::make_unique<MVLocalVQE>();
        state->threads = threads ? uint32_t(threads) : 2;
        const auto options = localvqe_options_new();
        if (!options) throw std::runtime_error("LocalVQE options allocation failed.");
        struct Options { localvqe_options_t value; ~Options() { localvqe_options_free(value); } } guard{options};
        if (localvqe_options_set_model_path(options, path) || localvqe_options_set_backend(options, "CPU")
            || localvqe_options_set_threads(options, int(state->threads)))
            throw std::runtime_error("LocalVQE configuration failed.");
        state->context = localvqe_new_with_options(options);
        if (!state->context) throw std::runtime_error("LocalVQE model could not be loaded or verified.");
        if (localvqe_sample_rate(state->context) != 16000 || localvqe_hop_length(state->context) != 256
            || localvqe_fft_size(state->context) != 512)
            throw std::runtime_error("Unexpected LocalVQE model audio format.");
        // No hard noise gate: avoid deliberately chopping quiet speech/tails.
        if (localvqe_set_noise_gate(state->context, 0, -45) != 0)
            throw std::runtime_error("Could not disable LocalVQE noise gate.");
        std::array<float, 256> silence{}, output{};
        for (int i = 0; i < 3; ++i)
            if (!MVLocalVQE::processHop(state.get(), silence.data(), output.data()))
                throw std::runtime_error("LocalVQE warm-up failed.");
        localvqe_reset(state->context);
        return state.release();
    } catch (const std::exception &e) {
        if (error && capacity) std::snprintf(error, capacity, "%s", e.what());
        return nullptr;
    } catch (...) {
        if (error && capacity) std::snprintf(error, capacity, "LocalVQE initialization failed.");
        return nullptr;
    }
}
extern "C" void mv_localvqe_destroy(MVLocalVQE *state) { delete state; }
extern "C" int mv_localvqe_reset(MVLocalVQE *state) {
    if (!state) return -1;
    try { localvqe_reset(state->context); state->adapter.reset(); return 0; }
    catch (...) { return -1; }
}
extern "C" int mv_localvqe_process(MVLocalVQE *state, const float *input, float *output) {
    if (!output) return -1;
    try {
        bool valid = state && input;
        if (valid) for (size_t i = 0; i < 480; ++i) valid = valid && std::isfinite(input[i]) && std::abs(input[i]) <= 1;
        if (valid && state->adapter.process(input, output, MVLocalVQE::processHop, state)) {
            for (size_t i = 0; i < 480; ++i) output[i] = std::clamp(output[i], -1.0f, 1.0f);
            return 0;
        }
    } catch (...) { }
    std::fill_n(output, 480, 0.0f);
    // No stale audio may reappear if a caller retries after malformed input.
    if (state) mv_localvqe_reset(state);
    return -1;
}
extern "C" uint32_t mv_localvqe_delay_frames(void) { return mv::LocalVQEAdapter::delay; }
extern "C" uint32_t mv_localvqe_threads(const MVLocalVQE *state) { return state ? state->threads : 0; }
