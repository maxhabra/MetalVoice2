#pragma once
#include <cstdint>
#include <memory>

namespace mv {
// One worker owns the complete chain; no extra queue between processors.
// Configure/clear only while stopped. Reset and process use the worker thread.
class NoisePipeline {
public:
    static constexpr uint32_t frameSize = 480;
    static constexpr uint32_t rnnoiseDelay = 960;
    static constexpr uint32_t deepFilterDelay = 1440;
    static constexpr uint32_t dpdfDelay = 2400;
    NoisePipeline();
    ~NoisePipeline();
    NoisePipeline(const NoisePipeline &) = delete;
    NoisePipeline &operator=(const NoisePipeline &) = delete;
    bool configure(int webRTCLevel, int enhancedMode, bool preferCoreML = false,
                   const char *modelPath = nullptr, int computeUnits = 0,
                   const char *initialStatePath = nullptr, bool roomDereverb = false);
    bool reset();
    void clear();
    bool process(float *samples);
    static uint32_t delayFrames(int webRTCLevel, int enhancedMode, bool roomDereverb = false);
    // 0 none, 1 native reference, 2 Core ML, 3 native after Core ML load failure.
    int deepFilterBackend() const;
    uint32_t computeDevices() const;
    uint32_t dpdfComputeDevices() const;
    uint32_t localVQEThreads() const;
private:
    struct State;
    std::unique_ptr<State> state;
};
}
