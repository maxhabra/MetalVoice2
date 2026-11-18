#include "RVCStream.hpp"
#include "mv_audio.h"
#include "Transport.hpp"
#include "CaptureQueue.hpp"
#include "NoisePipeline.hpp"
#include "RoomDereverb.hpp"
#include "mv_webrtc_aec.h"
#include "mv_playback_capture.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>
#include <mach/mach.h>
#include <pthread.h>
#include <thread>
#include <future>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <memory>
#include <stdexcept>

static_assert(std::atomic<double>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);

namespace {
constexpr UInt32 maxFrames = 8192;
constexpr size_t histogramSize = 5001; // 0.1 ms buckets for cleanup, 1 ms with voice conversion.
constexpr size_t computeHistogramSize = 1001; // 0.01 ms buckets, final bin >=10 ms.
AudioObjectPropertyAddress addr(AudioObjectPropertySelector selector,
                                AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    return {selector, scope, kAudioObjectPropertyElementMain};
}
template<class T> T property(AudioObjectID id, AudioObjectPropertySelector selector,
                             AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal, T fallback = {}) {
    auto a = addr(selector, scope); T value = fallback; UInt32 size = sizeof(T);
    if (AudioObjectGetPropertyData(id, &a, 0, nullptr, &size, &value) != noErr) return fallback;
    return value;
}
std::string stringProperty(AudioObjectID id, AudioObjectPropertySelector selector) {
    CFStringRef value = property<CFStringRef>(id, selector);
    if (!value) return {};
    char text[1024] = {};
    CFStringGetCString(value, text, sizeof(text), kCFStringEncodingUTF8);
    CFRelease(value);
    return text;
}
UInt32 channelCount(AudioDeviceID id, AudioObjectPropertyScope scope) {
    auto a = addr(kAudioDevicePropertyStreamConfiguration, scope);
    UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(id, &a, 0, nullptr, &bytes) != noErr || !bytes) return 0;
    std::vector<uint8_t> memory(bytes);
    auto list = reinterpret_cast<AudioBufferList *>(memory.data());
    if (AudioObjectGetPropertyData(id, &a, 0, nullptr, &bytes, list) != noErr) return 0;
    UInt32 count = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) count += list->mBuffers[i].mNumberChannels;
    return count;
}
struct DeviceRoute {
    AudioDeviceID id;
    std::string uid;
    UInt32 alive, channels, buffer;
    double rate;
    bool operator==(const DeviceRoute &) const = default;
};
DeviceRoute deviceRoute(AudioDeviceID id, AudioObjectPropertyScope scope) {
    return {id, stringProperty(id, kAudioDevicePropertyDeviceUID),
            property<UInt32>(id, kAudioDevicePropertyDeviceIsAlive), channelCount(id, scope),
            property<UInt32>(id, kAudioDevicePropertyBufferFrameSize),
            property<Float64>(id, kAudioDevicePropertyNominalSampleRate)};
}

class DeviceNotReady : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

double reportedLatency(AudioDeviceID id, AudioObjectPropertyScope scope, double rate) {
    UInt32 frames = property<UInt32>(id, kAudioDevicePropertyLatency, scope)
                  + property<UInt32>(id, kAudioDevicePropertySafetyOffset, scope);
    auto a = addr(kAudioDevicePropertyStreams, scope); UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(id, &a, 0, nullptr, &bytes) == noErr && bytes) {
        std::vector<AudioStreamID> streams(bytes / sizeof(AudioStreamID));
        if (AudioObjectGetPropertyData(id, &a, 0, nullptr, &bytes, streams.data()) == noErr) {
            UInt32 streamFrames = 0;
            for (auto stream : streams)
                streamFrames = std::max(streamFrames, property<UInt32>(stream, kAudioStreamPropertyLatency));
            frames += streamFrames;
        }
    }
    return frames * 1000.0 / rate;
}
void check(OSStatus status, const char *operation) {
    if (status == noErr) return;
    const auto message = std::string(operation) + " (Core Audio " + std::to_string(status) + ")";
    switch (status) {
        case kAudioHardwareNotReadyError:
        case kAudioHardwareNotRunningError:
        case kAudioHardwareBadObjectError:
        case kAudioHardwareBadDeviceError:
        case kAudioHardwareBadStreamError:
        case kAudioUnitErr_CannotDoInCurrentContext:
        case kAudioUnitErr_FailedInitialization:
            throw DeviceNotReady(message);
        default:
            throw std::runtime_error(message);
    }
}
AudioUnit newHAL() {
    AudioComponentDescription desc{kAudioUnitType_Output, kAudioUnitSubType_HALOutput,
                                   kAudioUnitManufacturer_Apple, 0, 0};
    auto component = AudioComponentFindNext(nullptr, &desc);
    if (!component) throw std::runtime_error("The Core Audio HAL output component is unavailable.");
    AudioUnit unit = nullptr;
    check(AudioComponentInstanceNew(component, &unit), "Create audio unit");
    return unit;
}
void set(AudioUnit unit, AudioUnitPropertyID key, AudioUnitScope scope, AudioUnitElement bus,
         const void *value, UInt32 bytes) {
    check(AudioUnitSetProperty(unit, key, scope, bus, value, bytes), "Configure audio unit");
}
AudioStreamBasicDescription format(double rate, UInt32 channels) {
    return {rate, kAudioFormatLinearPCM, UInt32(kAudioFormatFlagsNativeFloatPacked) | UInt32(kAudioFormatFlagIsNonInterleaved),
            sizeof(float), 1, sizeof(float), channels, 32, 0};
}
}

struct MVEngine {
    AudioUnit input = nullptr, output = nullptr;
    DeviceRoute inputRoute{}, outputRoute{}, referenceRoute{};
    std::atomic<bool> routeDirty{false};
    std::atomic<uint64_t> deviceListGeneration{0};
    double inRate = 0, outRate = 0, ticksToNS = 0;
    double inputDeviceLatency = 0, outputDeviceLatency = 0;
    UInt32 inChannels = 0, selectedChannel = 0, outChannels = 0, inBuffer = 0, outBuffer = 0;
    mv::Transport transport;
    mv::RVCStream voice;
    mv::CaptureQueue pendingCapture;
    int nsLevel = -1, enhancedMode = 0;
    bool preferCoreML = true, roomDereverb = false, echoCancellation = false;
    std::unique_ptr<MVPlaybackCapture, decltype(&mv_playback_destroy)> playback{nullptr, mv_playback_destroy};
    UInt32 referenceHeadroom = 0;
    std::atomic<int> echoState{0};
    std::atomic<double> echoERLE{-1}, echoPathDelay{-1};
    std::atomic<int> deepFilterBackend{0};
    std::atomic<uint32_t> deepFilterDevices{0};
    std::atomic<uint32_t> dpdfDevices{0}, localVQEThreads{0};
    semaphore_t workerWake = MACH_PORT_NULL;
    std::thread worker;
    std::atomic<bool> workerStop{false};
    std::atomic<uint64_t> nsBlocks{0}, nsNanos{0}, nsMaxNanos{0};
    std::array<std::atomic<uint64_t>, computeHistogramSize> computeHistogram{};
    std::vector<float> captureStorage;
    std::vector<uint8_t> captureListStorage;
    AudioBufferList *captureList = nullptr;
    std::array<float, maxFrames> mono{};
    struct Listener { AudioObjectID object; AudioObjectPropertyAddress address; };
    std::vector<Listener> listeners;
    std::atomic<bool> running{false}, changed{false}, muted{false}, latencyValid{false};
    std::atomic<int32_t> lastError{0};
    std::atomic<uint64_t> captured{0}, rendered{0}, underruns{0}, resyncs{0}, latencyCount{0};
    std::atomic<double> latency{0}, maxLatency{0}, queue{0}, ppm{0}, inputPeak{0}, outputPeak{0};
    std::array<std::atomic<uint64_t>, histogramSize> histogram{};
    float gain = 1;

    bool echoEnabled() const { return echoCancellation && enhancedMode != 4; }
    bool roomEnabled() const { return roomDereverb && enhancedMode != 4; }
    bool processingEnabled() const { return voice.enabled() || echoEnabled() || roomEnabled() || nsLevel >= 0 || enhancedMode != 0; }

    void processCapture(std::promise<bool> prepared) {
        pthread_setname_np("MetalVoice2 suppression");
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        // Tract's runtime state is not Send. Create, warm, process, reset and
        // destroy every processor on this same thread; only readiness crosses
        // to the control queue, before any hardware callback starts.
        mv::NoisePipeline pipeline;
        if (!pipeline.configure(nsLevel, enhancedMode, preferCoreML, nullptr, 0, nullptr, roomEnabled())) {
            prepared.set_value(false);
            return;
        }
        std::unique_ptr<MVEchoCanceller, decltype(&mv_aec_destroy)> aec(
            echoEnabled() ? mv_aec_create() : nullptr, mv_aec_destroy);
        if (echoEnabled() && !aec) { prepared.set_value(false); return; }
        deepFilterBackend = pipeline.deepFilterBackend();
        deepFilterDevices = pipeline.computeDevices();
        dpdfDevices = pipeline.dpdfComputeDevices();
        localVQEThreads = pipeline.localVQEThreads();
        const auto delayFrames = mv::NoisePipeline::delayFrames(nsLevel, enhancedMode, roomEnabled())
            + (aec ? mv_aec_delay_frames() : 0);
        const double cleanupDelayNS = delayFrames * 1e9 / inRate;
        prepared.set_value(true);
        std::array<float, 480> block{}, referenceLeft{}, referenceRight{};
        uint64_t previousDrops = 0, waitingSince = 0;
        double hostNS = 0;
        bool held = false, synchronized = false;
        const auto failReference = [&] {
            echoState = 3; lastError = MVErrorEchoReferenceUnavailable;
        };
        const auto reset = [&] {
            if (voice.enabled()) { lastError = MVErrorVoiceStream; return false; }
            if (!pipeline.reset()) { lastError = kAudio_MemFullError; return false; }
            if (aec && mv_aec_reset(aec.get()) != 0) {
                echoState = 3; lastError = MVErrorEchoProcessing; return false;
            }
            if (playback) mv_playback_discard(playback.get());
            held = false; synchronized = false; waitingSince = 0;
            echoState = echoEnabled() ? 1 : 0;
            transport.markDiscontinuity();
            return true;
        };
        while (!workerStop.load(std::memory_order_acquire)) {
            // A bounded wait detects a stalled reference even if its callback stops.
            const auto wakeResult = echoEnabled()
                ? semaphore_timedwait(workerWake, mach_timespec_t{0, 20000000})
                : semaphore_wait(workerWake);
            if (workerStop.load(std::memory_order_acquire)) break;
            if (wakeResult != KERN_SUCCESS && wakeResult != KERN_OPERATION_TIMED_OUT) continue;
            if (playback && captured.load() > 0) {
                MVPlaybackInfo info{}; mv_playback_info(playback.get(), &info);
                if (info.error || info.dropped || info.callback_age_ms > 250) { failReference(); return; }
                if (waitingSince && (mach_absolute_time() - waitingSince) * ticksToNS > 250e6) { failReference(); return; }
            }
            const auto drops = pendingCapture.overruns();
            if (drops != previousDrops) {
                previousDrops = drops;
                pendingCapture.discard();
                if (!reset()) return;
            }
            while (!workerStop.load(std::memory_order_acquire)) {
                if (!held) {
                    const auto result = pendingCapture.pop(block.data(), block.size(), hostNS);
                    if (result == mv::CaptureQueue::Read::waiting) break;
                    if (result == mv::CaptureQueue::Read::gap) {
                        if (!reset()) return;
                        continue;
                    }
                    held = true;
                }
                if (playback) {
                    if (hostNS <= 0) { failReference(); return; }
                    if (!waitingSince) waitingSince = mach_absolute_time();
                    const int reference = mv_playback_read(playback.get(), hostNS, referenceLeft.data(), referenceRight.data());
                    if (reference == 0) break; // Retain this mic block until its matching reference arrives.
                    if (reference < 0) {
                        if (synchronized) { failReference(); return; }
                        held = false; continue; // Startup: discard mic audio older than the first tap timestamp.
                    }
                    synchronized = true; waitingSince = 0; echoState = 2;
                }
                held = false;
                const auto before = mach_absolute_time();
                if (aec && mv_aec_process(aec.get(), referenceLeft.data(), referenceRight.data(), block.data()) != 0) {
                    echoState = 3; lastError = MVErrorEchoProcessing; return;
                }
                if (!pipeline.process(block.data())) { lastError.store(kAudioUnitErr_FailedInitialization); return; }
                const auto nanos = uint64_t((mach_absolute_time() - before) * ticksToNS);
                nsNanos.fetch_add(nanos, std::memory_order_relaxed);
                nsMaxNanos.store(std::max(nanos, nsMaxNanos.load(std::memory_order_relaxed)), std::memory_order_relaxed);
                computeHistogram[std::min(computeHistogramSize - 1, size_t(nanos / 10000))].fetch_add(1, std::memory_order_relaxed);
                const auto blocks = nsBlocks.fetch_add(1, std::memory_order_relaxed) + 1;
                if (aec && blocks % 100 == 0) {
                    double erle, delay; mv_aec_metrics(aec.get(), &erle, &delay);
                    echoERLE = erle; echoPathDelay = delay;
                }
                double origin = hostNS > 0 ? hostNS - cleanupDelayNS : 0;
                if (voice.enabled() && !voice.exchange(block.data(), origin, muted.load(std::memory_order_relaxed))) {
                    lastError = MVErrorVoiceStream;
                    return;
                }
                transport.push(block.data(), 480, origin);
            }
        }
    }

    bool activeRouteChanged() const {
        if (deviceRoute(inputRoute.id, kAudioObjectPropertyScopeInput) != inputRoute ||
            deviceRoute(outputRoute.id, kAudioObjectPropertyScopeOutput) != outputRoute) return true;
        if (!playback) return false;
        const auto id = property<AudioDeviceID>(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice);
        return id != referenceRoute.id || deviceRoute(id, kAudioObjectPropertyScopeOutput) != referenceRoute;
    }
    static OSStatus deviceEvent(AudioObjectID object, UInt32 count,
                                const AudioObjectPropertyAddress *addresses, void *context) {
        auto &engine = *static_cast<MVEngine *>(context);
        if (object == kAudioObjectSystemObject) {
            for (UInt32 i = 0; i < count; ++i)
                if (addresses[i].mSelector == kAudioHardwarePropertyDevices)
                    engine.deviceListGeneration.fetch_add(1, std::memory_order_relaxed);
        }
        engine.routeDirty.store(true, std::memory_order_release);
        return noErr;
    }
    void listen(AudioObjectID object, AudioObjectPropertySelector key,
                AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
        auto a = addr(key, scope);
        if (AudioObjectAddPropertyListener(object, &a, deviceEvent, this) == noErr)
            listeners.push_back({object, a});
    }
    static OSStatus capture(void *context, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *time,
                            UInt32, UInt32 count, AudioBufferList *) {
        auto &e = *static_cast<MVEngine *>(context);
        if (count > maxFrames) { e.lastError.store(kAudioUnitErr_TooManyFramesToProcess); return noErr; }
        for (UInt32 c = 0; c < e.inChannels; ++c) {
            e.captureList->mBuffers[c].mData = e.captureStorage.data() + c * maxFrames;
            e.captureList->mBuffers[c].mDataByteSize = count * sizeof(float);
        }
        const auto status = AudioUnitRender(e.input, flags, time, 1, count, e.captureList);
        if (status != noErr) { e.lastError.store(status); return noErr; }
        const auto samples = static_cast<const float *>(e.captureList->mBuffers[e.selectedChannel].mData);
        if (!samples) { e.lastError.store(kAudioUnitErr_InvalidPropertyValue); return noErr; }
        float peak = 0;
        if (*flags & kAudioUnitRenderAction_OutputIsSilence)
            std::fill_n(static_cast<float *>(e.captureList->mBuffers[e.selectedChannel].mData), count, 0.0f);
        else
            for (UInt32 i = 0; i < count; ++i) peak = std::max(peak, std::abs(samples[i]));
        e.inputPeak.store(peak, std::memory_order_relaxed);
        const double hostNS = time && (time->mFlags & kAudioTimeStampHostTimeValid)
            ? time->mHostTime * e.ticksToNS : 0;
        if (e.processingEnabled()) {
            e.pendingCapture.push(samples, count, hostNS);
            semaphore_signal(e.workerWake);
        } else {
            e.transport.push(samples, count, hostNS);
        }
        e.captured.fetch_add(count, std::memory_order_relaxed);
        return noErr;
    }
    static OSStatus render(void *context, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *time,
                           UInt32, UInt32 count, AudioBufferList *data) {
        auto &e = *static_cast<MVEngine *>(context);
        if (!data) return noErr;
        *flags &= ~kAudioUnitRenderAction_OutputIsSilence;
        if (count > maxFrames) {
            for (UInt32 c = 0; c < data->mNumberBuffers; ++c) {
                auto &b = data->mBuffers[c];
                if (b.mData) std::memset(b.mData, 0, b.mDataByteSize);
            }
            e.lastError.store(kAudioUnitErr_TooManyFramesToProcess);
            *flags |= kAudioUnitRenderAction_OutputIsSilence;
            return noErr;
        }
        const auto info = e.transport.render(e.mono.data(), count);
        e.queue.store(info.queueFrames * 1000.0 / e.inRate, std::memory_order_relaxed);
        e.ppm.store(info.correctionPPM, std::memory_order_relaxed);
        if (info.underrun) e.underruns.fetch_add(1, std::memory_order_relaxed);
        if (info.resync) e.resyncs.fetch_add(1, std::memory_order_relaxed);
        bool validTime = info.valid && info.sourceHostNS > 0 && time && (time->mFlags & kAudioTimeStampHostTimeValid);
        if (validTime) {
            const double ms = (time->mHostTime * e.ticksToNS - info.sourceHostNS) / 1e6;
            validTime = std::isfinite(ms) && ms >= 0 && ms < (e.voice.enabled() ? 5000 : 500);
            if (validTime) {
                e.latency.store(ms, std::memory_order_relaxed);
                e.maxLatency.store(std::max(ms, e.maxLatency.load(std::memory_order_relaxed)), std::memory_order_relaxed);
                e.histogram[std::min(histogramSize - 1, size_t(ms * (e.voice.enabled() ? 1 : 10)))].fetch_add(1, std::memory_order_relaxed);
                e.latencyCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
        e.latencyValid.store(validTime, std::memory_order_relaxed);
        const float wantedGain = e.muted.load(std::memory_order_relaxed) ? 0 : 1;
        float peak = 0;
        if (e.gain != wantedGain) {
            for (UInt32 i = 0; i < count; ++i) {
                e.gain += std::clamp(wantedGain - e.gain, -1.0f / 128, 1.0f / 128);
                e.mono[i] *= e.gain;
                peak = std::max(peak, std::abs(e.mono[i]));
            }
        } else if (wantedGain == 0) {
            std::fill_n(e.mono.data(), count, 0.0f);
        } else {
            for (UInt32 i = 0; i < count; ++i) peak = std::max(peak, std::abs(e.mono[i]));
        }
        for (UInt32 c = 0; c < data->mNumberBuffers; ++c) {
            auto &b = data->mBuffers[c];
            if (b.mData && b.mNumberChannels == 1 && b.mDataByteSize >= count * sizeof(float))
                std::memcpy(b.mData, e.mono.data(), count * sizeof(float));
            else if (b.mData) {
                std::memset(b.mData, 0, b.mDataByteSize);
                e.lastError.store(kAudioUnitErr_FormatNotSupported);
            }
        }
        if (peak == 0) *flags |= kAudioUnitRenderAction_OutputIsSilence;
        e.outputPeak.store(peak, std::memory_order_relaxed);
        e.rendered.fetch_add(count, std::memory_order_relaxed);
        return noErr;
    }
    void stop() {
        running.store(false);
        for (const auto &l : listeners) AudioObjectRemovePropertyListener(l.object, &l.address, deviceEvent, this);
        listeners.clear();
        if (output) { AudioOutputUnitStop(output); AudioUnitUninitialize(output); AudioComponentInstanceDispose(output); output = nullptr; }
        if (input) { AudioOutputUnitStop(input); AudioUnitUninitialize(input); AudioComponentInstanceDispose(input); input = nullptr; }
        if (playback) mv_playback_stop(playback.get());
        workerStop.store(true, std::memory_order_release);
        if (workerWake != MACH_PORT_NULL) semaphore_signal(workerWake);
        if (worker.joinable()) worker.join();
        voice.closeStream();
        playback.reset();
        if (workerWake != MACH_PORT_NULL) { semaphore_destroy(mach_task_self(), workerWake); workerWake = MACH_PORT_NULL; }
        latencyValid.store(false);
    }
    ~MVEngine() { stop(); }
};

extern "C" int mv_devices(MVDevice *devices, int capacity) {
    auto a = addr(kAudioHardwarePropertyDevices); UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &bytes) != noErr) return -1;
    std::vector<AudioDeviceID> ids(bytes / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &bytes, ids.data()) != noErr) return -1;
    const auto defaultInput = property<AudioDeviceID>(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultInputDevice);
    if (!devices || capacity == 0) return int(ids.size());
    const int count = std::min(capacity, int(ids.size()));
    for (int i = 0; i < count; ++i) {
        MVDevice d{}; d.id = ids[i];
        d.input_channels = channelCount(d.id, kAudioObjectPropertyScopeInput);
        d.output_channels = channelCount(d.id, kAudioObjectPropertyScopeOutput);
        d.sample_rate = property<Float64>(d.id, kAudioDevicePropertyNominalSampleRate);
        d.buffer_frames = property<UInt32>(d.id, kAudioDevicePropertyBufferFrameSize);
        d.is_virtual = property<UInt32>(d.id, kAudioDevicePropertyTransportType) == kAudioDeviceTransportTypeVirtual;
        d.is_default_input = d.id == defaultInput;
        d.alive = property<UInt32>(d.id, kAudioDevicePropertyDeviceIsAlive);
        std::snprintf(d.name, sizeof(d.name), "%s", stringProperty(d.id, kAudioObjectPropertyName).c_str());
        std::snprintf(d.uid, sizeof(d.uid), "%s", stringProperty(d.id, kAudioDevicePropertyDeviceUID).c_str());
        // Never offer our private playback reference as a microphone.
        if (std::string(d.uid).rfind("app.metalvoice2.echo-reference.", 0) == 0) d.alive = 0;
        devices[i] = d;
    }
    return count;
}
extern "C" MVEngine *mv_create() { return new MVEngine(); }
extern "C" void mv_destroy(MVEngine *e) { delete e; }
extern "C" void mv_stop(MVEngine *e) { if (e) e->stop(); }
extern "C" void mv_set_muted(MVEngine *e, int muted) { if (e) e->muted.store(muted != 0); }
extern "C" int mv_set_noise_suppression(MVEngine *e, int level) {
    if (!e || e->input || e->output || level < -1 || level > 3) return -1;
    e->nsLevel = level;
    return 0;
}
extern "C" int mv_set_enhanced_suppression(MVEngine *e, int mode) {
    if (!e || e->input || e->output || mode < 0 || mode > 4) return -1;
    e->enhancedMode = mode;
    return 0;
}
extern "C" int mv_set_echo_cancellation(MVEngine *e, int enabled) {
    if (!e || e->input || e->output || enabled < 0 || enabled > 1) return -1;
    e->echoCancellation = enabled != 0;
    return 0;
}
extern "C" int mv_set_room_dereverb(MVEngine *e, int enabled) {
    if (!e || e->input || e->output || enabled < 0 || enabled > 1) return -1;
    e->roomDereverb = enabled != 0;
    return 0;
}
extern "C" int mv_set_deepfilter_acceleration(MVEngine *e, int enabled) {
    if (!e || e->input || e->output || enabled < 0 || enabled > 1) return -1;
    e->preferCoreML = enabled != 0;
    return 0;
}

extern "C" void mv_set_voice(MVEngine *e, const char *python, const char *script, const char *config, uint32_t chunk_ms) {
    if (!e || e->input || e->output) return;
    e->voice.python = python ? python : "";
    e->voice.script = script ? script : "";
    e->voice.config = config ? config : "";
    e->voice.blocksPerChunk = std::max(1u, chunk_ms / 10);
}

extern "C" int mv_start(MVEngine *e, uint32_t inputID, uint32_t outputID, uint32_t channel,
                          char *error, size_t errorCapacity) {
    if (!e) return MVStartFailed;
    e->stop();
    int failureCode = MVStartFailed;
    try {
        if (inputID == outputID) throw std::runtime_error("Input and output must be different devices to avoid a feedback loop.");
        e->inputRoute = deviceRoute(inputID, kAudioObjectPropertyScopeInput);
        e->outputRoute = deviceRoute(outputID, kAudioObjectPropertyScopeOutput);
        for (const auto *route : {&e->inputRoute, &e->outputRoute}) {
            if (!route->alive || route->uid.empty() || !route->channels || !route->buffer || route->rate == 0)
                throw DeviceNotReady("The selected audio device is not ready.");
        }
        const auto transportType = property<UInt32>(outputID, kAudioDevicePropertyTransportType);
        if (!transportType) throw DeviceNotReady("The virtual output is not ready.");
        if (transportType != kAudioDeviceTransportTypeVirtual)
            throw std::runtime_error("Choose a virtual output such as BlackHole.");
        e->selectedChannel = channel;
        e->inChannels = e->inputRoute.channels;
        e->outChannels = std::min(UInt32(2), e->outputRoute.channels);
        e->inRate = e->inputRoute.rate;
        e->outRate = e->outputRoute.rate;
        e->inBuffer = e->inputRoute.buffer;
        e->outBuffer = e->outputRoute.buffer;
        if (!e->inChannels || e->inChannels > 64 || channel >= e->inChannels || !e->outChannels)
            throw std::runtime_error("The selected device channels are unavailable.");
        if (!std::isfinite(e->inRate) || !std::isfinite(e->outRate) || e->inRate < 8000 || e->inRate > 192000 || e->outRate < 8000 || e->outRate > 192000)
            throw std::runtime_error("This baseline supports device rates from 8 to 192 kHz.");
        if (e->processingEnabled() && e->inRate != 48000)
            throw std::runtime_error("Microphone filters and voice conversion require a 48 kHz microphone. Set it to 48,000 Hz in Audio MIDI Setup.");
        e->referenceHeadroom = 0;
        e->echoState = e->echoEnabled() ? 1 : 0; e->echoERLE = -1; e->echoPathDelay = -1;
        if (e->echoEnabled()) {
            char message[1024]{};
            e->playback.reset(mv_playback_create(message, sizeof(message)));
            if (!e->playback) throw std::runtime_error(message);
            MVPlaybackInfo info{}; mv_playback_info(e->playback.get(), &info);
            if (!info.buffer_frames || info.buffer_frames > maxFrames)
                throw std::runtime_error("Echo playback buffer is unavailable or too large.");
            e->referenceRoute = deviceRoute(info.device_id, kAudioObjectPropertyScopeOutput);
            e->referenceHeadroom = UInt32(std::ceil(info.buffer_frames * e->inRate / info.sample_rate));
        }
        const double block = std::max(double(e->inBuffer), e->outBuffer * e->inRate / e->outRate);
        const double sourceBlock = e->processingEnabled() ? std::max(block, 480.0) : block;
        // A worker-produced 480-frame block can arrive just after an output
        // callback. Reserve one processing hop so that phase changes between
        // the USB callback, worker, and virtual device do not drain the queue.
        const UInt32 workerHeadroom = e->processingEnabled() ? 480 : 0;
        const auto target = UInt32(std::ceil(std::max(sourceBlock * 2, e->inRate * 0.005)))
            + mv::Transport::filterLength(e->inRate, e->outRate) + workerHeadroom + e->referenceHeadroom;
        if (target > mv::Transport::capacity / 4 || !e->inBuffer || !e->outBuffer)
            throw std::runtime_error("The device buffer is too large or unavailable. Choose smaller buffers in the device settings.");
        e->transport.configure(e->inRate, e->outRate, target);
        mach_timebase_info_data_t timebase; mach_timebase_info(&timebase);
        e->ticksToNS = double(timebase.numer) / timebase.denom;
        e->inputDeviceLatency = reportedLatency(inputID, kAudioObjectPropertyScopeInput, e->inRate);
        e->outputDeviceLatency = reportedLatency(outputID, kAudioObjectPropertyScopeOutput, e->outRate);
        e->captureStorage.assign(e->inChannels * maxFrames, 0);
        e->captureListStorage.resize(offsetof(AudioBufferList, mBuffers) + e->inChannels * sizeof(AudioBuffer));
        e->captureList = reinterpret_cast<AudioBufferList *>(e->captureListStorage.data());
        e->captureList->mNumberBuffers = e->inChannels;
        for (UInt32 c = 0; c < e->inChannels; ++c)
            e->captureList->mBuffers[c] = {1, maxFrames * sizeof(float), e->captureStorage.data() + c * maxFrames};
        e->captured = 0; e->rendered = 0; e->underruns = 0; e->resyncs = 0; e->latencyCount = 0;
        e->latency = 0; e->maxLatency = 0; e->queue = 0; e->ppm = 0; e->inputPeak = 0; e->outputPeak = 0;
        e->lastError = 0; e->changed = false; e->gain = e->muted ? 0 : 1;
        e->deepFilterBackend = 0; e->deepFilterDevices = 0; e->dpdfDevices = 0; e->localVQEThreads = 0;
        if (e->voice.enabled()) e->voice.start();
        e->pendingCapture.reset(e->inRate);
        e->nsBlocks = 0; e->nsNanos = 0; e->nsMaxNanos = 0;
        for (auto &bin : e->computeHistogram) bin = 0;
        if (e->processingEnabled()) {
            if (semaphore_create(mach_task_self(), &e->workerWake, SYNC_POLICY_FIFO, 0) != KERN_SUCCESS)
                throw std::runtime_error("Could not create the audio worker signal.");
            e->workerStop.store(false, std::memory_order_release);
            std::promise<bool> prepared;
            auto ready = prepared.get_future();
            e->worker = std::thread([e, prepared = std::move(prepared)]() mutable {
                e->processCapture(std::move(prepared));
            });
            if (!ready.get())
                throw std::runtime_error("Could not prepare the selected noise suppression processors.");
        }
        for (auto &bin : e->histogram) bin = 0;
        const UInt32 yes = 1, no = 0;
        e->input = newHAL();
        set(e->input, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &yes, sizeof(yes));
        set(e->input, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &no, sizeof(no));
        set(e->input, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &inputID, sizeof(inputID));
        auto inputFormat = format(e->inRate, e->inChannels);
        set(e->input, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &inputFormat, sizeof(inputFormat));
        set(e->input, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof(maxFrames));
        AURenderCallbackStruct inputCallback{MVEngine::capture, e};
        set(e->input, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &inputCallback, sizeof(inputCallback));
        check(AudioUnitInitialize(e->input), "Initialize microphone");
        e->output = newHAL();
        set(e->output, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &no, sizeof(no));
        set(e->output, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &outputID, sizeof(outputID));
        auto outputFormat = format(e->outRate, e->outChannels);
        set(e->output, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &outputFormat, sizeof(outputFormat));
        set(e->output, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof(maxFrames));
        AURenderCallbackStruct outputCallback{MVEngine::render, e};
        set(e->output, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &outputCallback, sizeof(outputCallback));
        check(AudioUnitInitialize(e->output), "Initialize virtual output");
        for (auto id : {inputID, outputID}) {
            e->listen(id, kAudioDevicePropertyDeviceIsAlive);
            e->listen(id, kAudioDevicePropertyNominalSampleRate);
            e->listen(id, kAudioDevicePropertyBufferFrameSize);
        }
        e->listen(inputID, kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput);
        e->listen(outputID, kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput);
        if (e->playback) {
            MVPlaybackInfo info{}; mv_playback_info(e->playback.get(), &info);
            e->listen(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice);
            e->listen(info.device_id, kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput);
            e->listen(info.device_id, kAudioDevicePropertyDeviceIsAlive);
            e->listen(info.device_id, kAudioDevicePropertyNominalSampleRate);
            e->listen(info.device_id, kAudioDevicePropertyBufferFrameSize);
            char message[1024]{};
            if (mv_playback_start(e->playback.get(), e->workerWake, message, sizeof(message)) != 0)
                throw std::runtime_error(message);
        }
        e->listen(kAudioObjectSystemObject, kAudioHardwarePropertyDevices);
        check(AudioOutputUnitStart(e->input), "Start microphone");
        check(AudioOutputUnitStart(e->output), "Start virtual output");
        e->routeDirty = true; // Also check events between taking snapshots and installing listeners.
        e->running = true;
        return MVStartOK;
    } catch (const DeviceNotReady &failure) {
        failureCode = MVStartDeviceNotReady;
        if (error && errorCapacity) std::snprintf(error, errorCapacity, "%s", failure.what());
    } catch (const std::exception &failure) {
        if (error && errorCapacity) std::snprintf(error, errorCapacity, "%s", failure.what());
    }
    e->stop();
    return failureCode;
}

extern "C" MVMeterLevels mv_meter_levels(const MVEngine *e) {
    return {e->inputPeak.load(std::memory_order_relaxed), e->outputPeak.load(std::memory_order_relaxed),
            e->voice.displayRMS.load(std::memory_order_relaxed)};
}

extern "C" void mv_stats(MVEngine *e, MVStats *s) {
    if (!e || !s) return;
    *s = {};
    s->running = e->running; s->latency_valid = e->latencyValid;
    if (s->running && e->routeDirty.exchange(false, std::memory_order_acq_rel) && e->activeRouteChanged())
        e->changed = true;
    s->device_list_generation = e->deviceListGeneration.load(std::memory_order_relaxed);
    s->device_changed = e->changed; s->last_error = e->lastError;
    s->input_rate = e->inRate; s->output_rate = e->outRate;
    s->input_buffer_frames = e->inBuffer; s->output_buffer_frames = e->outBuffer;
    s->queue_ms = e->queue; s->target_queue_ms = e->inRate ? e->transport.target() * 1000.0 / e->inRate : 0;
    s->latency_ms = e->latency; s->latency_max_ms = e->maxLatency; s->latency_samples = e->latencyCount;
    s->drift_ppm = e->ppm; s->input_peak = e->inputPeak; s->output_peak = e->outputPeak;
    s->voice_input_rms = e->voice.inputRMS.load(std::memory_order_relaxed);
    s->input_device_latency_ms = e->inputDeviceLatency; s->output_device_latency_ms = e->outputDeviceLatency;
    s->input_frames = e->captured; s->output_frames = e->rendered;
    s->underruns = e->underruns; s->overruns = e->transport.overruns() + e->pendingCapture.overruns(); s->resyncs = e->resyncs;
    s->noise_suppression_level = e->nsLevel;
    s->enhanced_suppression = e->enhancedMode;
    s->deepfilter_backend = e->deepFilterBackend;
    s->deepfilter_compute_devices = e->deepFilterDevices;
    s->dpdf_compute_devices = e->dpdfDevices;
    s->dpdf_delay_ms = e->enhancedMode == 3 ? mv::NoisePipeline::dpdfDelay / 48.0 : 0;
    s->webrtc_delay_ms = mv::NoisePipeline::delayFrames(e->nsLevel, false) / 48.0;
    s->rnnoise_delay_ms = e->enhancedMode == 1 ? mv::NoisePipeline::rnnoiseDelay / 48.0 : 0;
    s->deepfilter_delay_ms = e->enhancedMode == 2 ? mv::NoisePipeline::deepFilterDelay / 48.0 : 0;
    s->room_dereverb = e->roomEnabled();
    s->room_dereverb_delay_ms = e->roomEnabled() ? mv::RoomDereverb::delayFrames / 48.0 : 0;
    s->echo_cancellation = e->echoEnabled(); s->echo_reference_state = e->echoState;
    s->echo_delay_ms = e->echoEnabled() ? mv_aec_delay_frames() / 48.0 : 0;
    s->echo_reference_headroom_ms = e->referenceHeadroom / 48.0;
    s->echo_erle_db = e->echoERLE; s->echo_estimated_path_delay_ms = e->echoPathDelay;
    if (e->playback) {
        MVPlaybackInfo info{}; mv_playback_info(e->playback.get(), &info);
        s->echo_reference_peak = info.peak; s->echo_reference_age_ms = info.callback_age_ms;
        s->echo_reference_frames = info.frames; s->echo_reference_drops = info.dropped;
        std::snprintf(s->echo_reference_device, sizeof(s->echo_reference_device), "%s", info.device_name);
    }
    s->localvqe_delay_ms = e->enhancedMode == 4 ? mv::NoisePipeline::delayFrames(-1, 4) / 48.0 : 0;
    s->localvqe_threads = e->localVQEThreads;
    s->suppression_delay_ms = s->echo_delay_ms + s->room_dereverb_delay_ms + s->webrtc_delay_ms + s->rnnoise_delay_ms + s->deepfilter_delay_ms + s->dpdf_delay_ms + s->localvqe_delay_ms;
    s->suppression_blocks = e->nsBlocks;
    s->suppression_mean_ms = s->suppression_blocks ? double(e->nsNanos.load()) / s->suppression_blocks / 1e6 : 0;
    s->suppression_max_ms = e->nsMaxNanos.load() / 1e6;
    uint64_t computed = 0;
    for (size_t i = 0; i < computeHistogramSize && s->suppression_blocks; ++i) {
        computed += e->computeHistogram[i].load(std::memory_order_relaxed);
        if (computed >= uint64_t(std::ceil(s->suppression_blocks * 0.99))) {
            // Overflow bin uses the observed maximum, never a capped 10 ms.
            s->suppression_p99_ms = i == computeHistogramSize - 1 ? s->suppression_max_ms : (i + 1) / 100.0;
            break;
        }
    }
    uint64_t total = 0;
    const auto rank50 = uint64_t(std::ceil(s->latency_samples * 0.50));
    const auto rank95 = uint64_t(std::ceil(s->latency_samples * 0.95));
    const int binsPerMS = e->voice.enabled() ? 1 : 10;
    for (size_t i = 0; i < histogramSize && s->latency_samples; ++i) {
        const auto previous = total;
        total += e->histogram[i].load(std::memory_order_relaxed);
        if (previous < rank50 && total >= rank50) s->latency_p50_ms = (i + 0.5) / binsPerMS;
        if (total >= rank95) {
            s->latency_p95_ms = (i + 0.5) / binsPerMS;
            break;
        }
    }
}
