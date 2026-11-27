#include "mv_audio.h"
#include <AudioToolbox/AudioToolbox.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <thread>
#include <mach/mach_time.h>
#include <dlfcn.h>
#include <memory>
#include <stdexcept>

void routeTests() {
    auto event = reinterpret_cast<void (*)(bool)>(dlsym(RTLD_DEFAULT, "mv_fixture_event"));
    auto tick = reinterpret_cast<UInt32 (*)(float *)>(dlsym(RTLD_DEFAULT, "mv_fixture_tick"));
    auto setMicAvailable = reinterpret_cast<void (*)(bool)>(dlsym(RTLD_DEFAULT, "mv_fixture_set_mic_available"));
    auto setDefaultMic = reinterpret_cast<void (*)(AudioObjectID)>(dlsym(RTLD_DEFAULT, "mv_fixture_set_default_mic"));
    auto failOpen = reinterpret_cast<void (*)(OSStatus)>(dlsym(RTLD_DEFAULT, "mv_fixture_fail_open"));
    auto require = [](bool ok, const char *why) { if (!ok) throw std::runtime_error(why); };
    require(event && tick && setMicAvailable && setDefaultMic && failOpen, "Synthetic HAL is required");
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
    setDefaultMic(303);
    mv_stats(engine.get(), &before);
    require(before.device_list_generation > after.device_list_generation, "Default-only change must refresh the microphone list");
    MVDevice devices[3]{};
    require(mv_devices(devices, 3) == 3, "Expected both microphones and virtual output");
    auto defaultMic = std::find_if(devices, devices + 3, [](const auto &d) { return d.is_default_input; });
    require(defaultMic != devices + 3 && defaultMic->id == 303 && !defaultMic->is_virtual, "Expected new physical default");
    setMicAvailable(false);
    require(mv_start(engine.get(), defaultMic->id, 302, 0, error, sizeof(error)) == MVStartOK, "Replacement mic failed to open");
    for (int i = 0; i < 8; ++i) tick(output.data());
    require(std::abs(output[256] - .25f) < .001f, "New microphone audio must reach the same virtual output");
    mv_stop(engine.get());
    const auto stoppedGeneration = mv_device_list_generation(engine.get());
    setMicAvailable(true);
    setDefaultMic(301);
    require(mv_device_list_generation(engine.get()) >= stoppedGeneration + 2, "Detect reconnect and new default while stopped");
    require(start() == MVStartOK, "Original mic failed to reopen");
    for (int i = 0; i < 8; ++i) tick(output.data());
    mv_set_muted(engine.get(), 1);
    const auto flags = tick(output.data());
    require(!(flags & kAudioUnitRenderAction_OutputIsSilence) && output[0] > .49f &&
            output[126] > 0 && output[127] == 0, "Mute transition was lost");
    require((tick(output.data()) & kAudioUnitRenderAction_OutputIsSilence) &&
            std::all_of(output.begin(), output.end(), [](float x){ return x == 0; }), "Muted output must be silent");
    mv_set_muted(engine.get(), 0);
    require(!(tick(output.data()) & kAudioUnitRenderAction_OutputIsSilence) &&
            output[0] > 0 && output[0] < .01f && output[127] > .49f, "Unmute must fade back to full gain");
    tick(output.data());
    auto meters = mv_meter_levels(engine.get());
    require(meters.output_peak > .49 && meters.output_peak < .51, "Unity gain must preserve the output meter");
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
    mv_stop(engine.get());
    mv_set_muted(engine.get(), 0);
    mv_set_voice(engine.get(), "/usr/bin/python3", "Tests/RouteFixture/voice_worker.py", "{}", 100);
    require(start() == MVStartOK, "Voice worker failed to start");
    require(mv_voice_speaker_count(engine.get()) == 3, "Expected speaker count from the worker");
    auto audio = reinterpret_cast<void (*)(const float *, uint64_t, float *)>(dlsym(RTLD_DEFAULT, "mv_fixture_audio"));
    require(audio, "Audio injection fixture missing");
    mach_timebase_info_data_t timebase{};
    mach_timebase_info(&timebase);
    auto origin = mach_absolute_time();
    auto deadline = std::chrono::steady_clock::now();
    std::array<float, 512> in{};
    for (size_t i = 0; i < in.size(); ++i) in[i] = (i % 256 < 128) ? .4f : .2f;
    bool converted = false;
    for (int i = 0; i < 90; ++i) {
        audio(in.data(), origin + uint64_t(i * 512e9 / 48000 * timebase.denom / timebase.numer), output.data());
        require(*std::max_element(output.begin(), output.end()) < .05f, "Original voice leaked while converting");
        converted |= *std::min_element(output.begin(), output.end()) < -.35f;
        deadline += std::chrono::microseconds(10667);
        std::this_thread::sleep_until(deadline);
    }
    mv_stats(engine.get(), &after);
    require(after.voice_input_rms > .315 && after.voice_input_rms < .318, "Gate meter must use chunk RMS");
    auto levels = mv_meter_levels(engine.get());
    require(levels.voice_rms > .30 && levels.voice_rms < .33, "Live voice input level missing");
    in.fill(0);
    for (int i = 90; i < 94; ++i) {
        audio(in.data(), origin + uint64_t(i * 512e9 / 48000 * timebase.denom / timebase.numer), output.data());
        deadline += std::chrono::microseconds(10667);
        std::this_thread::sleep_until(deadline);
    }
    require(mv_meter_levels(engine.get()).voice_rms < levels.voice_rms * .8,
            "Input meter must react before a 100 ms conversion chunk finishes");
    require(converted, "Voice output did not reach the virtual device");
    mv_stop(engine.get());
    mv_set_voice(engine.get(), nullptr, nullptr, nullptr, 0);
    require(start() == MVStartOK, "Bypass failed to restart");
    for (int i = 0; i < 12; ++i) tick(output.data());
    mv_stats(engine.get(), &after);
    require(after.voice_input_rms == 0, "Bypass must clear the gate meter");
    require(output[256] > .45f, "Bypass did not restore input");
    std::puts("PASS post-cleanup voice routing and bypass");
    std::puts("PASS simulated default microphone handoff, HAL reopen, unrelated hot-plug, buffer change and mute flags");
}
