// A test process loads this dylib before AudioCore. It simulates two microphones and a virtual output
// and drives the registered callbacks without opening a microphone or speaker.
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <algorithm>
#include <cstring>
#include <mach/mach_time.h>
#include <type_traits>
#include <vector>

namespace {
constexpr AudioObjectID mic = 301, output = 302, secondMic = 303;
AudioObjectID defaultMic = mic;
UInt32 bufferSize = 512;
bool micAvailable = true;
OSStatus nextOpen = noErr;
struct Unit { AudioObjectID device{}; AURenderCallbackStruct capture{}, render{}; };
struct Listener {
    AudioObjectID object;
    AudioObjectPropertyAddress address;
    AudioObjectPropertyListenerProc fn;
    void *context;
};
std::vector<Listener> listeners;
Unit *source = nullptr, *sink = nullptr;
const float *fixtureAudio = nullptr;
uint64_t fixtureTime = 0;

OSStatus propertySize(AudioObjectID id, const AudioObjectPropertyAddress *address,
                      UInt32, const void *, UInt32 *bytes) {
    if (id == kAudioObjectSystemObject && address->mSelector == kAudioHardwarePropertyDevices) {
        *bytes = (micAvailable ? 3 : 2) * sizeof(AudioObjectID);
        return noErr;
    }
    if ((id == mic || id == secondMic || id == output) && address->mSelector == kAudioDevicePropertyStreamConfiguration) {
        *bytes = sizeof(AudioBufferList);
        return noErr;
    }
    return kAudioHardwareUnknownPropertyError;
}
OSStatus propertyData(AudioObjectID id, const AudioObjectPropertyAddress *address,
                      UInt32, const void *, UInt32 *bytes, void *data) {
    if (id == kAudioObjectSystemObject) {
        if (address->mSelector == kAudioHardwarePropertyDefaultInputDevice) {
            *static_cast<AudioObjectID *>(data) = defaultMic;
            return noErr;
        }
        if (address->mSelector == kAudioHardwarePropertyDevices) {
            const AudioObjectID ids[] = {output, secondMic, mic};
            const UInt32 size = (micAvailable ? 3 : 2) * sizeof(AudioObjectID);
            if (*bytes < size) return kAudioHardwareBadPropertySizeError;
            std::memcpy(data, ids, size);
            *bytes = size;
            return noErr;
        }
    }
    if ((id != mic && id != secondMic && id != output) || (id == mic && !micAvailable)) return kAudioHardwareBadObjectError;
    switch (address->mSelector) {
        case kAudioObjectPropertyName:
        case kAudioDevicePropertyDeviceUID: {
            CFStringRef uid = CFSTR("fixture.output");
            if (id == mic) uid = CFSTR("fixture.mic");
            if (id == secondMic) uid = CFSTR("fixture.secondMic");
            *static_cast<CFStringRef *>(data) = static_cast<CFStringRef>(CFRetain(uid));
            break;
        }
        case kAudioDevicePropertyNominalSampleRate: *static_cast<double *>(data) = 48000; break;
        case kAudioDevicePropertyBufferFrameSize: *static_cast<UInt32 *>(data) = bufferSize; break;
        case kAudioDevicePropertyDeviceIsAlive: *static_cast<UInt32 *>(data) = 1; break;
        case kAudioDevicePropertyTransportType:
            *static_cast<UInt32 *>(data) = id == output ? kAudioDeviceTransportTypeVirtual : kAudioDeviceTransportTypeUSB;
            break;
        case kAudioDevicePropertyStreamConfiguration: {
            auto *list = static_cast<AudioBufferList *>(data);
            if (*bytes < sizeof(*list)) return kAudioHardwareBadPropertySizeError;
            const auto scope = id == output ? kAudioObjectPropertyScopeOutput : kAudioObjectPropertyScopeInput;
            list->mNumberBuffers = address->mScope == scope ? 1 : 0;
            list->mBuffers[0] = {1, 0, nullptr};
            break;
        }
        default: return kAudioHardwareUnknownPropertyError;
    }
    return noErr;
}
OSStatus addListener(AudioObjectID id, const AudioObjectPropertyAddress *a, AudioObjectPropertyListenerProc fn, void *context) {
    listeners.push_back({id, *a, fn, context});
    return noErr;
}
OSStatus removeListener(AudioObjectID id, const AudioObjectPropertyAddress *a, AudioObjectPropertyListenerProc fn, void *context) {
    std::erase_if(listeners, [&](const Listener &l) {
        return l.object == id && l.address.mSelector == a->mSelector && l.address.mScope == a->mScope && l.fn == fn && l.context == context;
    });
    return noErr;
}
AudioComponent component(AudioComponent, const AudioComponentDescription *) { return reinterpret_cast<AudioComponent>(1); }
OSStatus create(AudioComponent, AudioComponentInstance *unit) {
    if (nextOpen) { const auto err = nextOpen; nextOpen = noErr; return err; }
    *unit = reinterpret_cast<AudioComponentInstance>(new Unit);
    return noErr;
}
OSStatus dispose(AudioComponentInstance unit) {
    auto *u = reinterpret_cast<Unit *>(unit);
    if (u == source) source = nullptr;
    if (u == sink) sink = nullptr;
    delete u;
    return noErr;
}
OSStatus configure(AudioUnit unit, AudioUnitPropertyID key, AudioUnitScope, AudioUnitElement, const void *data, UInt32) {
    auto *u = reinterpret_cast<Unit *>(unit);
    if (key == kAudioOutputUnitProperty_CurrentDevice) {
        u->device = *static_cast<const AudioObjectID *>(data);
    } else if (key == kAudioOutputUnitProperty_SetInputCallback) {
        u->capture = *static_cast<const AURenderCallbackStruct *>(data);
        source = u;
    } else if (key == kAudioUnitProperty_SetRenderCallback) {
        u->render = *static_cast<const AURenderCallbackStruct *>(data);
        sink = u;
    }
    return noErr;
}
OSStatus success(AudioUnit) { return noErr; }
OSStatus inputAudio(AudioUnit unit, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32, UInt32 n, AudioBufferList *list) CA_REALTIME_API {
    auto *dest = static_cast<float *>(list->mBuffers[0].mData);
    if (fixtureAudio) std::copy_n(fixtureAudio, n, dest);
    else std::fill_n(dest, n, reinterpret_cast<Unit *>(unit)->device == secondMic ? .25f : .5f);
    return noErr;
}
}
extern "C" void mv_fixture_event(bool activeBufferChange) {
    const AudioObjectID id = activeBufferChange ? mic : kAudioObjectSystemObject;
    const auto key = activeBufferChange ? UInt32(kAudioDevicePropertyBufferFrameSize) : UInt32(kAudioHardwarePropertyDevices);
    if (activeBufferChange) bufferSize = 256;
    for (const auto &l : listeners)
        if (l.object == id && l.address.mSelector == key) l.fn(id, 1, &l.address, l.context);
}
extern "C" void mv_fixture_set_default_mic(AudioObjectID id) {
    defaultMic = id;
    for (const auto &l : listeners)
        if (l.object == kAudioObjectSystemObject && l.address.mSelector == kAudioHardwarePropertyDefaultInputDevice)
            l.fn(l.object, 1, &l.address, l.context);
}
extern "C" void mv_fixture_set_mic_available(bool available) {
    micAvailable = available;
    mv_fixture_event(false);
}
extern "C" void mv_fixture_fail_open(OSStatus status) { nextOpen = status; }
extern "C" UInt32 mv_fixture_tick(float *pcm) {
    AudioTimeStamp time{};
    time.mFlags = kAudioTimeStampHostTimeValid;
    time.mHostTime = fixtureTime ? fixtureTime : mach_absolute_time();
    AudioUnitRenderActionFlags flags = 0;
    source->capture.inputProc(source->capture.inputProcRefCon, &flags, &time, 1, 512, nullptr);
    AudioBufferList list{1, {{1, 512 * sizeof(float), pcm}}};
    sink->render.inputProc(sink->render.inputProcRefCon, &flags, &time, 0, 512, &list);
    return flags;
}

extern "C" void mv_fixture_audio(const float *input, uint64_t hostTime, float *output) {
    fixtureAudio = input;
    fixtureTime = hostTime;
    mv_fixture_tick(output);
    fixtureAudio = nullptr;
    fixtureTime = 0;
}

#define REPLACE(symbol, replacement) \
    static_assert(std::is_same_v<decltype(&symbol), decltype(&replacement)>); \
    __attribute__((used, section("__DATA,__interpose"))) \
    static const struct { decltype(&symbol) mock, real; } hook_##symbol = {replacement, symbol};
REPLACE(AudioObjectGetPropertyDataSize, propertySize)
REPLACE(AudioObjectGetPropertyData, propertyData)
REPLACE(AudioObjectAddPropertyListener, addListener)
REPLACE(AudioObjectRemovePropertyListener, removeListener)
REPLACE(AudioComponentFindNext, component)
REPLACE(AudioComponentInstanceNew, create)
REPLACE(AudioComponentInstanceDispose, dispose)
REPLACE(AudioUnitSetProperty, configure)
REPLACE(AudioUnitInitialize, success)
REPLACE(AudioUnitUninitialize, success)
REPLACE(AudioOutputUnitStart, success)
REPLACE(AudioOutputUnitStop, success)
REPLACE(AudioUnitRender, inputAudio)
