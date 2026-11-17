#pragma once
#include <cstddef>
#include <memory>

namespace mv {
// Single-channel, causal room dereverberation. Owned by the capture worker.
// No allocation or locks in process/reset. 48 kHz, 10 ms input/output blocks.
class RoomDereverb {
public:
    static constexpr size_t hop = 480, delayFrames = 480;
    RoomDereverb();
    ~RoomDereverb();
    RoomDereverb(const RoomDereverb &) = delete;
    RoomDereverb &operator=(const RoomDereverb &) = delete;
    void reset();
    bool process(float *samples);
private:
    struct State;
    std::unique_ptr<State> state;
};
}
