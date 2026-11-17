#include "mv_playback_capture.h"
#include <algorithm>
#include "EchoReferenceQueue.hpp"
#import <Foundation/Foundation.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/CATapDescription.h>
#import <CoreAudio/AudioHardwareTapping.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <unistd.h>
#include <memory>
#include <string>
#include <stdexcept>
#include <cstdio>
#include <thread>

namespace {
AudioObjectPropertyAddress address(AudioObjectPropertySelector selector, AudioObjectPropertyScope scope=kAudioObjectPropertyScopeGlobal) {
    return {selector,scope,kAudioObjectPropertyElementMain};
}
void check(OSStatus status, const char *operation) {
    if (status!=noErr) throw std::runtime_error(std::string(operation)+" (Core Audio "+std::to_string(status)+")");
}
template<class T> T get(AudioObjectID object, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope=kAudioObjectPropertyScopeGlobal) {
    T value{}; UInt32 size=sizeof(value); auto key=address(selector,scope);
    check(AudioObjectGetPropertyData(object,&key,0,nullptr,&size,&value),"Read playback device"); return value;
}
NSString *stringProperty(AudioObjectID object, AudioObjectPropertySelector selector) {
    CFStringRef value=get<CFStringRef>(object,selector);
    return CFBridgingRelease(value);
}
void failure(char *error,size_t capacity,const char *message) {
    if(error && capacity) std::snprintf(error,capacity,"%s",message);
}
struct CallbackState {
    mv::EchoReferenceQueue queue;
    double rate=48000,ticksToNS=0;
    uint32_t channels=2;
    bool planar=false;
    semaphore_t wake=MACH_PORT_NULL;
    std::atomic<bool> enabled{false};
    std::atomic<uint32_t> activeCallbacks{0};
    std::atomic<uint64_t> frames{0},lastCallback{0};
    std::atomic<double> peak{0};
    std::atomic<int32_t> error{0};
};
OSStatus callback(AudioDeviceID,const AudioTimeStamp*,const AudioBufferList *input,
                  const AudioTimeStamp *time,AudioBufferList*,const AudioTimeStamp*,void *context) {
    auto &s=*static_cast<CallbackState*>(context);
    s.activeCallbacks.fetch_add(1);
    struct Exit { CallbackState &state; ~Exit(){state.activeCallbacks.fetch_sub(1);} } exit{s};
    if(!s.enabled.load()) return noErr;
    s.lastCallback.store(mach_absolute_time(),std::memory_order_relaxed);
    if(!input || !time || !(time->mFlags&kAudioTimeStampHostTimeValid) || !time->mHostTime
        || input->mNumberBuffers != (s.planar ? s.channels : 1) || !input->mBuffers[0].mData) {
        s.error.store(-1); return noErr;
    }
    const auto &first=input->mBuffers[0];
    const auto *left=static_cast<const float*>(first.mData);
    const float *right=left;
    uint32_t stride=first.mNumberChannels;
    if(!stride || (s.planar ? stride!=1 : stride!=s.channels)) {s.error.store(-2);return noErr;}
    uint32_t count=first.mDataByteSize/(sizeof(float)*stride);
    if(!count || count>8192 || first.mDataByteSize% (sizeof(float)*stride)) {s.error.store(-3);return noErr;}
    if(s.channels==2) {
        if(s.planar) {
            if(input->mNumberBuffers!=2 || input->mBuffers[1].mNumberChannels!=1
               || input->mBuffers[1].mDataByteSize!=first.mDataByteSize || !input->mBuffers[1].mData) {
                s.error.store(-4); return noErr;
            }
            right=static_cast<const float*>(input->mBuffers[1].mData);
        } else right=left+1;
    }
    float peak=0;
    for(uint32_t i=0;i<count;++i) peak=std::max({peak,std::abs(left[i*stride]),std::abs(right[i*stride])});
    s.peak.store(peak,std::memory_order_relaxed);
    s.queue.push(left,right,count,stride,time->mHostTime*s.ticksToNS,s.rate);
    s.frames.fetch_add(count,std::memory_order_relaxed);
    if(s.wake!=MACH_PORT_NULL) semaphore_signal(s.wake);
    return noErr;
}
}
struct MVPlaybackCapture {
    AudioObjectID tap=0,aggregate=0,device=0;
    AudioDeviceIOProcID proc=nullptr;
    uint32_t bufferFrames=0;
    std::string name;
    CATapDescription *description=nil;
    std::unique_ptr<CallbackState> callbackState=std::make_unique<CallbackState>();
    bool teardownFailed=false, started=false;
    void stop() {
        callbackState->enabled.store(false);
        if(started && proc && aggregate && AudioDeviceStop(aggregate,proc)!=noErr) teardownFailed=true;
        started=false;
        while(callbackState->activeCallbacks.load()!=0) std::this_thread::yield();
    }
    ~MVPlaybackCapture() {
        stop();
        if(proc && aggregate && AudioDeviceDestroyIOProcID(aggregate,proc)!=noErr) teardownFailed=true;
        if(aggregate && AudioHardwareDestroyAggregateDevice(aggregate)!=noErr) teardownFailed=true;
        if(tap) AudioHardwareDestroyProcessTap(tap);
        // A failed OS teardown can leave a callback alive. Its disabled context
        // owns its queue and touches no engine memory; quarantine it rather
        // than risk a use-after-free. Successful teardown frees everything.
        if(teardownFailed) callbackState.release();
    }
};
extern "C" MVPlaybackCapture *mv_playback_create(char *error,size_t capacity) {
    @autoreleasepool { @try { try {
        auto s=std::make_unique<MVPlaybackCapture>();
        s->device=get<AudioDeviceID>(kAudioObjectSystemObject,kAudioHardwarePropertyDefaultOutputDevice);
        const auto transport=get<UInt32>(s->device,kAudioDevicePropertyTransportType);
        if(!s->device || transport==kAudioDeviceTransportTypeVirtual || transport==kAudioDeviceTransportTypeAggregate)
            throw std::runtime_error("Echo needs a physical macOS sound output. Choose your speakers or headphones in Sound settings.");
        NSString *uid=stringProperty(s->device,kAudioDevicePropertyDeviceUID);
        s->name=stringProperty(s->device,kAudioObjectPropertyName).UTF8String;
        pid_t pid=getpid();AudioObjectID process=0;UInt32 bytes=sizeof(process);
        auto translate=address(kAudioHardwarePropertyTranslatePIDToProcessObject);
        const auto translated=AudioObjectGetPropertyData(kAudioObjectSystemObject,&translate,sizeof(pid),&pid,&bytes,&process);
        NSArray<NSNumber*> *excluded=translated==noErr && process ? @[@(process)] : @[];
        s->description=[[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:excluded];
        s->description.deviceUID=uid;
        s->description.stream=@0;
        s->description.privateTap=YES;
        s->description.muteBehavior=CATapUnmuted;
        s->description.name=@"MetalVoice2 Echo reference";
        check(AudioHardwareCreateProcessTap(s->description,&s->tap),"Create Echo playback reference");
        if(!s->tap) throw std::runtime_error("Echo playback device is unavailable.");
        NSDictionary *properties=@{
            @kAudioAggregateDeviceNameKey:@"MetalVoice2 Echo reference",
            @kAudioAggregateDeviceUIDKey:[@"app.metalvoice2.echo-reference." stringByAppendingString:NSUUID.UUID.UUIDString],
            @kAudioAggregateDeviceIsPrivateKey:@YES,
            @kAudioAggregateDeviceTapAutoStartKey:@NO,
            @kAudioAggregateDeviceTapListKey:@[@{
                @kAudioSubTapUIDKey:s->description.UUID.UUIDString,
                @kAudioSubTapDriftCompensationKey:@YES
            }]
        };
        check(AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)properties,&s->aggregate),"Create Echo reference device");
        Float64 rate=48000;auto rateKey=address(kAudioDevicePropertyNominalSampleRate);
        check(AudioObjectSetPropertyData(s->aggregate,&rateKey,0,nullptr,sizeof(rate),&rate),"Set Echo reference rate");
        UInt32 frames=128;auto bufferKey=address(kAudioDevicePropertyBufferFrameSize);
        // This private tap-only aggregate does not reconfigure the physical device.
        AudioObjectSetPropertyData(s->aggregate,&bufferKey,0,nullptr,sizeof(frames),&frames);
        const auto format=get<AudioStreamBasicDescription>(s->aggregate,kAudioDevicePropertyStreamFormat,kAudioObjectPropertyScopeInput);
        if(format.mFormatID!=kAudioFormatLinearPCM || !(format.mFormatFlags&kAudioFormatFlagIsFloat)
           || (format.mFormatFlags&kAudioFormatFlagIsBigEndian) || format.mBitsPerChannel!=32
           || format.mSampleRate!=48000 || format.mChannelsPerFrame<1 || format.mChannelsPerFrame>2)
            throw std::runtime_error("Echo requires a mono/stereo 48 kHz playback tap. Select a compatible macOS sound output.");
        const UInt32 bytesPerFrame=sizeof(float)*((format.mFormatFlags&kAudioFormatFlagIsNonInterleaved)?1:format.mChannelsPerFrame);
        if(format.mBytesPerFrame!=bytesPerFrame || format.mFramesPerPacket!=1 || format.mBytesPerPacket!=bytesPerFrame)
            throw std::runtime_error("Echo playback tap returned an unsupported PCM layout.");
        auto &c=*s->callbackState;
        c.rate=format.mSampleRate;c.channels=format.mChannelsPerFrame;
        c.planar=(format.mFormatFlags&kAudioFormatFlagIsNonInterleaved)!=0;
        mach_timebase_info_data_t timebase;mach_timebase_info(&timebase);
        c.ticksToNS=double(timebase.numer)/timebase.denom;
        s->bufferFrames=get<UInt32>(s->aggregate,kAudioDevicePropertyBufferFrameSize);
        check(AudioDeviceCreateIOProcID(s->aggregate,callback,s->callbackState.get(),&s->proc),
              "Allow MetalVoice2 in System Settings → Privacy & Security → Screen & System Audio Recording, then retry Echo");
        // macOS can create a tap yet return silence after permission denial.
        // A description write is the permission probe also used by Chromium.
        auto descriptionKey=address(kAudioTapPropertyDescription);
        CATapDescription *description=s->description;
        check(AudioObjectSetPropertyData(s->tap,&descriptionKey,0,nullptr,sizeof(description),&description),
              "Echo needs System Audio Recording permission for MetalVoice2");
        return s.release();
    } catch(const std::exception &e) {failure(error,capacity,e.what());return nullptr;}
    } @catch(NSException *e) {failure(error,capacity,e.reason.UTF8String);return nullptr;} }
}
extern "C" int mv_playback_start(MVPlaybackCapture *s,uint32_t wake,char *error,size_t capacity) {
    if(!s)return -1;
    s->callbackState->wake=wake;s->callbackState->enabled.store(true,std::memory_order_release);
    s->started=true;
    const auto result=AudioDeviceStart(s->aggregate,s->proc);
    if(result!=noErr) {s->stop();failure(error,capacity,"Could not start Echo reference. Check System Audio Recording permission and the macOS sound output.");}
    return result;
}
extern "C" void mv_playback_stop(MVPlaybackCapture *s) {if(s)s->stop();}
extern "C" void mv_playback_destroy(MVPlaybackCapture *s) {@autoreleasepool {delete s;}}
extern "C" int mv_playback_read(MVPlaybackCapture *s,double hostNS,float *left,float *right) {
    return s ? s->callbackState->queue.readBlock(hostNS,left,right) : -1;
}
extern "C" void mv_playback_discard(MVPlaybackCapture *s) {if(s)s->callbackState->queue.discard();}
extern "C" void mv_playback_info(MVPlaybackCapture *s,MVPlaybackInfo *info) {
    if(!s || !info)return;
    *info={};auto &c=*s->callbackState;
    info->device_id=s->device;info->buffer_frames=s->bufferFrames;info->sample_rate=c.rate;
    info->frames=c.frames.load();info->dropped=c.queue.overruns();info->peak=c.peak.load();info->error=c.error.load();
    const auto last=c.lastCallback.load();
    info->callback_age_ms=last ? (mach_absolute_time()-last)*c.ticksToNS/1e6 : -1;
    std::snprintf(info->device_name,sizeof(info->device_name),"%s",s->name.c_str());
}
