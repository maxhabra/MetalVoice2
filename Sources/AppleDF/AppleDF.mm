#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>
#include "mv_apple_df.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

struct MVAppleDF {
    MLModel *model = nil;
    MLState *history = nil;
    MLMultiArray *erb = nil;
    MLMultiArray *complex = nil;
    MLMultiArray *explicitHistory = nil;
    MLDictionaryFeatureProvider *features = nil;
    MLPredictionOptions *options = nil;
    uint32_t devices = 0;
};

#include "CoreMLHelpers.hpp"

extern "C" MVAppleDF *mv_apple_df_create(const char *path, int units, char *error, size_t capacity) {
    @autoreleasepool {
        @try {
            NSURL *url = path ? [NSURL fileURLWithPath:@(path)] : [NSBundle.mainBundle URLForResource:@"DeepFilterNet3" withExtension:@"mlmodelc"];
            if (!url) { errorText(error, capacity, @"Bundled DeepFilterNet3 Core ML model unavailable"); return nullptr; }
            NSError *failure = nil;
            if ([url.pathExtension isEqualToString:@"mlpackage"]) {
                url = [MLModel compileModelAtURL:url error:&failure];
                if (!url) { errorText(error, capacity, failure.localizedDescription); return nullptr; }
            }
            MLModelConfiguration *configuration = [MLModelConfiguration new];
            switch (units) {
                case 0: configuration.computeUnits = MLComputeUnitsAll; break;
                case 1: configuration.computeUnits = MLComputeUnitsCPUOnly; break;
                case 2: configuration.computeUnits = MLComputeUnitsCPUAndGPU; break;
                case 3: configuration.computeUnits = MLComputeUnitsCPUAndNeuralEngine; break;
                default: errorText(error, capacity, @"Invalid compute-unit selection"); return nullptr;
            }
            MLOptimizationHints *hints = [MLOptimizationHints new];
            hints.specializationStrategy = MLSpecializationStrategyFastPrediction;
            configuration.optimizationHints = hints;
            auto result = std::make_unique<MVAppleDF>();
            result->model = [MLModel modelWithContentsOfURL:url configuration:configuration error:&failure];
            if (!result->model) { errorText(error, capacity, failure.localizedDescription); return nullptr; }
            const bool stateful = result->model.modelDescription.stateDescriptionsByName.count > 0;
            if (stateful) result->history = [result->model newState];
            else {
                result->explicitHistory = [[MLMultiArray alloc] initWithShape:@[@1, @26304] dataType:MLMultiArrayDataTypeFloat32 error:&failure];
                if (result->explicitHistory) std::memset(result->explicitHistory.dataPointer, 0, 26304*sizeof(float));
            }
            result->erb = [[MLMultiArray alloc] initWithShape:@[@1, @1, @1, @32] dataType:MLMultiArrayDataTypeFloat32 error:&failure];
            result->complex = [[MLMultiArray alloc] initWithShape:@[@1, @2, @1, @96] dataType:MLMultiArrayDataTypeFloat32 error:&failure];
            if (!result->erb || !result->complex || (!result->history && !result->explicitHistory)) { errorText(error, capacity, @"Could not allocate Core ML buffers"); return nullptr; }
            NSMutableDictionary *features = [@{
                @"feat_erb": [MLFeatureValue featureValueWithMultiArray:result->erb],
                @"feat_spec": [MLFeatureValue featureValueWithMultiArray:result->complex]
            } mutableCopy];
            if (result->explicitHistory) features[@"history"] = [MLFeatureValue featureValueWithMultiArray:result->explicitHistory];
            result->features = [[MLDictionaryFeatureProvider alloc] initWithDictionary:features error:&failure];
            result->options = [MLPredictionOptions new];
            if (!result->features) { errorText(error, capacity, failure.localizedDescription); return nullptr; }
            result->devices = modelDevices(url, configuration);
            // Warm prediction before capture starts, then discard all history.
            float erb[32]{}, complex[192]{}, gains[32]{}, coefficients[960]{}, snr = 0;
            for (size_t i = 0; i < 192; ++i) complex[i] = 0.05f * std::sin(float(i) * 0.04f);
            for (int i = 0; i < 8; ++i) {
                if (mv_apple_df_predict(result.get(), erb, complex, gains, coefficients, &snr) != 0) {
                    errorText(error, capacity, @"Core ML warm-up failed"); return nullptr;
                }
            }
            if (mv_apple_df_reset(result.get()) != 0) { errorText(error, capacity, @"Core ML reset failed"); return nullptr; }
            return result.release();
        } @catch (NSException *exception) {
            errorText(error, capacity, exception.reason);
            return nullptr;
        }
    }
}

extern "C" void mv_apple_df_destroy(MVAppleDF *state) { @autoreleasepool { delete state; } }
extern "C" uint32_t mv_apple_df_devices(const MVAppleDF *state) { return state ? state->devices : 0; }
extern "C" int mv_apple_df_reset(MVAppleDF *state) {
    if (!state) return -1;
    @autoreleasepool { @try {
        if (state->explicitHistory) { std::memset(state->explicitHistory.dataPointer, 0, 26304*sizeof(float)); return 0; }
        state->history = [state->model newState]; return state->history ? 0 : -1;
    } @catch (NSException *) { return -1; } }
}
extern "C" int mv_apple_df_predict(void *context, const float *erb, const float *complex, float *gains, float *coefficients, float *lsnr) {
    auto *state = static_cast<MVAppleDF *>(context);
    if (!state || !erb || !complex || !gains || !coefficients || !lsnr) return -1;
    @autoreleasepool {
        @try {
            std::memcpy(state->erb.dataPointer, erb, 32 * sizeof(float));
            std::memcpy(state->complex.dataPointer, complex, 192 * sizeof(float));
            NSError *error = nil;
            id<MLFeatureProvider> result = state->history
                ? [state->model predictionFromFeatures:state->features usingState:state->history options:state->options error:&error]
                : [state->model predictionFromFeatures:state->features options:state->options error:&error];
            if (!result || error) return -1;
            if (state->explicitHistory && !copyOutput([result featureValueForName:@"next_history"].multiArrayValue, static_cast<float *>(state->explicitHistory.dataPointer), 26304)) return -1;
            return copyOutput([result featureValueForName:@"gains"].multiArrayValue, gains, 32)
                && copyOutput([result featureValueForName:@"coefs"].multiArrayValue, coefficients, 960)
                && copyOutput([result featureValueForName:@"lsnr"].multiArrayValue, lsnr, 1) ? 0 : -1;
        } @catch (NSException *) { return -1; }
    }
}
