#include "mv_dpdf.h"
#include "CoreMLHelpers.hpp"
#import <Accelerate/Accelerate.h>
#include <algorithm>
#include <array>
#include <memory>
#include <numbers>

namespace {
constexpr size_t hop = 480, windowSize = 960, bins = 481, stateSize = 56436;
}

struct MVDPDF {
    MLModel *model = nil;
    MLMultiArray *spectrum = nil, *history = nil;
    MLDictionaryFeatureProvider *features = nil;
    MLPredictionOptions *options = nil;
    std::array<float, stateSize> initial{};
    std::array<float, windowSize> window{}, input{}, overlap{}, real{}, imag{}, fftReal{}, fftImag{};
    std::array<float, bins * 2> spec{}, enhanced{};
    vDSP_DFT_Setup forward = nullptr, inverse = nullptr;
    uint32_t devices = 0;
    bool primed = false, failed = false;
    ~MVDPDF() {
        if (forward) vDSP_DFT_DestroySetup(forward);
        if (inverse) vDSP_DFT_DestroySetup(inverse);
    }
};

extern "C" int mv_dpdf_predict(MVDPDF *s, const float *spectrum, float *output) {
    if (!s || !spectrum || !output || s->failed) return -1;
    for (size_t i = 0; i < bins * 2; ++i) if (!std::isfinite(spectrum[i])) return -1;
    @autoreleasepool { @try {
        std::memcpy(s->spectrum.dataPointer, spectrum, bins * 2 * sizeof(float));
        NSError *error = nil;
        id<MLFeatureProvider> result = [s->model predictionFromFeatures:s->features options:s->options error:&error];
        if (result && !error && copyOutput([result featureValueForName:@"spec_e"].multiArrayValue, output, bins * 2)
            && copyOutput([result featureValueForName:@"state_out"].multiArrayValue,
                          static_cast<float *>(s->history.dataPointer), stateSize)) return 0;
    } @catch (NSException *) {} }
    s->failed = true;
    std::fill_n(output, bins * 2, 0.f);
    return -1;
}

extern "C" MVDPDF *mv_dpdf_create(const char *path, const char *statePath, int units, char *error, size_t capacity) {
    @autoreleasepool { @try {
        NSURL *url = path ? [NSURL fileURLWithPath:@(path)] : [NSBundle.mainBundle URLForResource:@"DPDFNet2" withExtension:@"mlmodelc"];
        NSURL *initialURL = statePath ? [NSURL fileURLWithPath:@(statePath)] : [NSBundle.mainBundle URLForResource:@"DPDFNet2.state" withExtension:@"f32"];
        if (!url || !initialURL) { errorText(error, capacity, @"Bundled DPDFNet-2 resources unavailable"); return nullptr; }
        NSError *failure = nil;
        NSData *initial = [NSData dataWithContentsOfURL:initialURL options:0 error:&failure];
        if (initial.length != stateSize * sizeof(float)) { errorText(error, capacity, @"Invalid DPDFNet-2 initial state"); return nullptr; }
        MLModelConfiguration *config = [MLModelConfiguration new];
        switch (units) {
            // Qualified on M1 Max: automatic placement exceeds a 10 ms hop;
            // CPU-only completes around 1.2 ms with identical Float32 output.
            case 0: config.computeUnits = MLComputeUnitsCPUOnly; break;
            case 1: config.computeUnits = MLComputeUnitsCPUOnly; break;
            case 2: config.computeUnits = MLComputeUnitsCPUAndGPU; break;
            case 3: config.computeUnits = MLComputeUnitsCPUAndNeuralEngine; break;
            case 4: config.computeUnits = MLComputeUnitsAll; break;
            default: errorText(error, capacity, @"Invalid compute-unit selection"); return nullptr;
        }
        MLOptimizationHints *hints = [MLOptimizationHints new];
        hints.specializationStrategy = MLSpecializationStrategyFastPrediction;
        config.optimizationHints = hints;
        auto s = std::make_unique<MVDPDF>();
        std::memcpy(s->initial.data(), initial.bytes, initial.length);
        for (float v : s->initial) if (!std::isfinite(v)) { errorText(error, capacity, @"Nonfinite DPDFNet-2 state"); return nullptr; }
        s->model = [MLModel modelWithContentsOfURL:url configuration:config error:&failure];
        if (!s->model) { errorText(error, capacity, failure.localizedDescription); return nullptr; }
        NSDictionary *inputs = s->model.modelDescription.inputDescriptionsByName;
        if (![inputs[@"spec"] multiArrayConstraint] || ![inputs[@"state_in"] multiArrayConstraint]
            || ![[inputs[@"spec"] multiArrayConstraint].shape isEqual:@[@1,@1,@481,@2]]
            || ![[inputs[@"state_in"] multiArrayConstraint].shape isEqual:@[@56436]]) {
            errorText(error, capacity, @"Unexpected DPDFNet-2 model signature"); return nullptr;
        }
        s->spectrum = [[MLMultiArray alloc] initWithShape:@[@1,@1,@481,@2] dataType:MLMultiArrayDataTypeFloat32 error:&failure];
        s->history = [[MLMultiArray alloc] initWithShape:@[@56436] dataType:MLMultiArrayDataTypeFloat32 error:&failure];
        if (!s->spectrum || !s->history) { errorText(error, capacity, @"Could not allocate DPDFNet-2 buffers"); return nullptr; }
        s->features = [[MLDictionaryFeatureProvider alloc] initWithDictionary:@{
            @"spec": [MLFeatureValue featureValueWithMultiArray:s->spectrum],
            @"state_in": [MLFeatureValue featureValueWithMultiArray:s->history]} error:&failure];
        s->options = [MLPredictionOptions new];
        s->forward = vDSP_DFT_zop_CreateSetup(nullptr, windowSize, vDSP_DFT_FORWARD);
        s->inverse = vDSP_DFT_zop_CreateSetup(nullptr, windowSize, vDSP_DFT_INVERSE);
        if (!s->features || !s->forward || !s->inverse) { errorText(error, capacity, @"Could not initialize DPDFNet-2 DSP"); return nullptr; }
        for (size_t i = 0; i < windowSize; ++i) {
            double x = std::sin(std::numbers::pi * (double(i) + 0.5) / windowSize);
            s->window[i] = float(std::sin(0.5 * std::numbers::pi * x * x));
        }
        // CPU-only is an explicit constraint, so no expensive graph inspection
        // is needed on the stream's startup path. Other units are test options.
        s->devices = units == 0 || units == 1 ? 1 : 0;
        if (mv_dpdf_reset(s.get()) != 0) return nullptr;
        for (int i = 0; i < 8; ++i) if (mv_dpdf_predict(s.get(), s->spec.data(), s->enhanced.data()) != 0) {
            errorText(error, capacity, @"DPDFNet-2 warm-up failed"); return nullptr;
        }
        mv_dpdf_reset(s.get());
        return s.release();
    } @catch (NSException *e) { errorText(error, capacity, e.reason); return nullptr; } }
}

extern "C" void mv_dpdf_destroy(MVDPDF *s) { @autoreleasepool { delete s; } }
extern "C" uint32_t mv_dpdf_devices(const MVDPDF *s) { return s ? s->devices : 0; }
extern "C" int mv_dpdf_reset(MVDPDF *s) {
    if (!s || !s->history.dataPointer) return -1;
    std::memcpy(s->history.dataPointer, s->initial.data(), stateSize * sizeof(float));
    s->input.fill(0); s->overlap.fill(0); s->imag.fill(0);
    s->primed = false; s->failed = false;
    return 0;
}
extern "C" int mv_dpdf_process(MVDPDF *s, const float *input, float *output) {
    if (!s || !input || !output) return -1;
    if (s->failed) { std::fill_n(output, hop, 0.f); return -1; }
    for (size_t i = 0; i < hop; ++i) if (!std::isfinite(input[i])) {
        std::fill_n(output, hop, 0.f); return -1;
    }
    std::copy_n(input, hop, s->input.data() + hop);
    if (!s->primed) {
        s->primed = true;
        std::copy_n(s->input.data() + hop, hop, s->input.data());
        std::fill_n(output, hop, 0.f);
        return 0;
    }
    vDSP_vmul(s->input.data(), 1, s->window.data(), 1, s->real.data(), 1, windowSize);
    s->imag.fill(0);
    vDSP_DFT_Execute(s->forward, s->real.data(), s->imag.data(), s->fftReal.data(), s->fftImag.data());
    for (size_t i = 0; i < bins; ++i) { s->spec[i*2] = s->fftReal[i]; s->spec[i*2+1] = s->fftImag[i]; }
    if (mv_dpdf_predict(s, s->spec.data(), s->enhanced.data()) != 0) { std::fill_n(output, hop, 0.f); return -1; }
    for (size_t i = 0; i < bins; ++i) { s->fftReal[i] = s->enhanced[i*2]; s->fftImag[i] = s->enhanced[i*2+1]; }
    s->fftImag[0] = s->fftImag[hop] = 0;
    for (size_t i = bins; i < windowSize; ++i) { s->fftReal[i] = s->fftReal[windowSize-i]; s->fftImag[i] = -s->fftImag[windowSize-i]; }
    vDSP_DFT_Execute(s->inverse, s->fftReal.data(), s->fftImag.data(), s->real.data(), s->imag.data());
    for (size_t i = 0; i < hop; ++i) {
        output[i] = s->real[i] * s->window[i] / windowSize + s->overlap[i];
        s->overlap[i] = s->real[i+hop] * s->window[i+hop] / windowSize;
    }
    std::copy_n(s->input.data() + hop, hop, s->input.data());
    return 0;
}
