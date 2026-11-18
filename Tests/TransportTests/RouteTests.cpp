#include "mv_audio.h"
#include <AudioToolbox/AudioToolbox.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <dlfcn.h>
#include <memory>
#include <stdexcept>

void routeTests() {
    auto event = reinterpret_cast<void (*)(bool)>(dlsym(RTLD_DEFAULT, "mv_fixture_event"));
    auto tick = reinterpret_cast<UInt32 (*)(float *)>(dlsym(RTLD_DEFAULT, "mv_fixture_tick"));
    auto setMicAvailable = reinterpret_cast<void (*)(bool)>(dlsym(RTLD_DEFAULT, "mv_fixture_set_mic_available"));
    auto failOpen = reinterpret_cast<void (*)(OSStatus)>(dlsym(RTLD_DEFAULT, "mv_fixture_fail_open"));
    auto require = [](bool ok, const char *why) { if (!ok) throw std::runtime_error(why); };
    require(event && tick && setMicAvailable && failOpen, "Synthetic HAL is required");
    std::unique_ptr<MVEngine, decltype(&mv_destroy)> engine(mv_create(), mv_destroy);
    char error[256]{};
    auto start = [&] { return mv_start(engine.get(), 301, 302, 0, error, sizeof(error)); };
    failOpen(kAudioDevicePermissionsError);
    require(start() == MVStartFailed, "Permission failure must stop retries");
    failOpen(kAudioHardwareNotReadyError);
    require(start() == MVStartDeviceNotReady, "Transient HAL error must allow a retry");
    require(start() == MVStartOK, "Reopened device must recover");
    std::array<float, 512> output{};
    for (int i = 0; i < 8; ++i) tick(output.data());
    MVStats before{}, after{};
    mv_stats(engine.get(), &before);
    event(false);
    tick(output.data());
    mv_stats(engine.get(), &after);
    require(!after.device_changed && after.device_list_generation > before.device_list_generation &&
            after.output_frames == before.output_frames + 512, "Unrelated device interrupted the active route");
    mv_set_muted(engine.get(), 1);
    const auto flags = tick(output.data());
    require(!(flags & kAudioUnitRenderAction_OutputIsSilence) && output[0] > .49f &&
            output[126] > 0 && output[127] == 0, "Mute transition was lost");
    require((tick(output.data()) & kAudioUnitRenderAction_OutputIsSilence) &&
            std::all_of(output.begin(), output.end(), [](float x){ return x == 0; }), "Muted output must be silent");
    event(true);
    mv_stats(engine.get(), &after);
    require(after.device_changed, "Active buffer change must rebuild the route");
    require(start() == MVStartOK, "Changed buffer must be usable");
    setMicAvailable(false);
    mv_stats(engine.get(), &after);
    require(after.device_changed, "Selected mic removal via the global list must rebuild the route");
    require(start() == MVStartDeviceNotReady, "Removed device should allow a retry");
    setMicAvailable(true);
    require(start() == MVStartOK, "Reconnected mic should reopen");
    std::puts("PASS simulated HAL reopen, unrelated hot-plug, selected buffer change and mute flags");
}
