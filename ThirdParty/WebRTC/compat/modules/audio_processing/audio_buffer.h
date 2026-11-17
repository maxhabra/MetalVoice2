#pragma once
// MetalVoice2 standalone integration, not the complete WebRTC AudioBuffer API.
// NoiseSuppressor only accesses the pre-split bands. The adapter owns mono,
// 48 kHz / 10 ms storage; upstream ThreeBandFilterBank performs split/merge.
#include <array>
#include <cassert>
#include <cstddef>

namespace webrtc {
class AudioBuffer {
public:
    std::array<std::array<float, 160>, 3> bands{};
    AudioBuffer() : pointers{bands[0].data(), bands[1].data(), bands[2].data()} {}
    AudioBuffer(const AudioBuffer&) = delete;
    AudioBuffer& operator=(const AudioBuffer&) = delete;
    float* const* split_bands(size_t channel) { assert(channel == 0); return pointers.data(); }
    const float* const* split_bands_const(size_t channel) const { assert(channel == 0); return pointers.data(); }
private:
    std::array<float*, 3> pointers;
};
}
