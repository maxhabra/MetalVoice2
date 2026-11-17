#include "RoomDereverb.hpp"
#include "NoisePipeline.hpp"
#include "mv_audio.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

static void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
static std::vector<float> read(const char *path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    require(bool(file), "Fixture unavailable");
    size_t size = file.tellg(); require(size && size % (480*sizeof(float)) == 0, "Invalid fixture size");
    std::vector<float> values(size/sizeof(float)); file.seekg(0); file.read(reinterpret_cast<char *>(values.data()), size);
    require(bool(file), "Fixture read failed"); return values;
}
int main(int argc, char **argv) { try {
    require(argc == 6, "Usage: room-tests INPUT REFERENCE DFN_MODEL DPDF_MODEL DPDF_STATE");
    const auto input = read(argv[1]), reference = read(argv[2]);
    require(input.size() == reference.size(), "Fixture length differs");
    auto track = reinterpret_cast<void (*)(int)>(dlsym(RTLD_DEFAULT,"mv_test_track_allocations"));
    auto allocations = reinterpret_cast<size_t (*)()>(dlsym(RTLD_DEFAULT,"mv_test_allocation_count"));
#if !__has_feature(address_sanitizer)
    require(track && allocations, "Run with allocation tracker");
#endif
    mv::RoomDereverb room;
    auto result = input;
    std::vector<double> times(input.size()/480);
    const size_t beforeAllocations = allocations ? allocations() : 0;
    for (size_t b=0; b<times.size(); ++b) {
        if (track) track(1);
        const auto before = std::chrono::steady_clock::now();
        const bool ok = room.process(result.data()+b*480);
        const auto after = std::chrono::steady_clock::now();
        if (track) track(0);
        require(ok,"Room processing failed");
        times[b]=std::chrono::duration<double,std::milli>(after-before).count();
    }
    if (allocations) require(allocations() == beforeAllocations, "Room processor allocated while streaming");
    double error=0, energy=0, maximum=0;
    for (size_t i=0; i<result.size(); ++i) {
        require(std::isfinite(result[i]),"Nonfinite output");
        double d=result[i]-reference[i]; error+=d*d; energy+=reference[i]*reference[i]; maximum=std::max(maximum,std::abs(d));
    }
    std::cout<<"NARA-WPE/NumPy parity: max "<<maximum<<", relative RMS "<<std::sqrt(error/energy)<<"\n";
    require(maximum < 3e-5 && std::sqrt(error/energy) < 3e-5,"Room output differs from independent oracle");
    room.reset(); auto repeated=input;
    for (size_t i=0;i<repeated.size();i+=480) require(room.process(repeated.data()+i),"Reset failed");
    require(repeated == result,"Reset must reproduce fresh output exactly");
    room.reset(); std::array<float,960> impulse{}; impulse[47]=.5f;
    require(room.process(impulse.data()) && room.process(impulse.data()+480),"Impulse processing failed");
    for (size_t i=0;i<impulse.size();++i) require(std::abs(impulse[i]-(i==527?.5f:0.f)) < 2e-6,"Incorrect 480-sample delay or startup gain");
    std::array<float,480> block{};
    block[19]=std::numeric_limits<float>::quiet_NaN(); require(!room.process(block.data()),"NaN must fail closed");
    for(float value:block) require(value==0,"Malformed input must produce silence");
    require(!room.process(nullptr),"Null input must fail");
    require(room.process(block.data()),"Must recover after bad input");
    for(float value:block) require(value==0,"Bad input left stale audio history");
    // Long changing levels, voiced periodicity, silence and full-scale noise.
    std::mt19937 random(72); std::uniform_real_distribution<float> noise(-1,1);
    for(int b=0;b<12000;++b) {
        const float gain = b%1200<400 ? 0.f : b%1200<800 ? .00001f : .8f;
        for(auto &v:block) v=gain*noise(random);
        require(room.process(block.data()),"Long stream failed");
        for(float v:block) require(std::isfinite(v) && std::abs(v)<2,"Long stream unstable");
    }
    // Every selectable chain, including disabled options, must keep ordering,
    // resetting and reported timestamp delay coherent. No device is opened.
    for (int mode=0;mode<4;++mode) for(int level=-1;level<4;++level) {
        mv::NoisePipeline cascade, denoiser;
        const char *model = mode==3 ? argv[4] : argv[3];
        require(cascade.configure(level,mode,true,model,1,argv[5],true),"Cascade init failed");
        require(denoiser.configure(level,mode,true,model,1,argv[5]),"Denoiser init failed");
        room.reset();
        for(size_t b=0;b<40;++b) {
            std::array<float,480> expected{}, actual{};
            std::copy_n(input.data()+b*480,480,expected.data());actual=expected;
            require(room.process(expected.data()) && denoiser.process(expected.data()) && cascade.process(actual.data()),"Cascade processing failed");
            for(size_t i=0;i<480;++i) require(std::abs(actual[i]-expected[i])<2e-6,"Room/WebRTC/enhanced order mismatch");
        }
        require(cascade.reset(),"Cascade reset failed");
        require(mv::NoisePipeline::delayFrames(level,mode,true)==mv::NoisePipeline::delayFrames(level,mode)+480,"Cascade delay missing room stage");
    }
    auto *engine=mv_create();
    require(mv_set_room_dereverb(engine,-1)!=0 && mv_set_room_dereverb(engine,2)!=0,"Invalid room option accepted");
    require(mv_set_room_dereverb(engine,1)==0,"Room option rejected");
    MVStats stats{};mv_stats(engine,&stats);
    require(stats.room_dereverb==1 && stats.room_dereverb_delay_ms==10 && stats.suppression_delay_ms==10,"Room-only stats/delay incorrect");
    require(mv_set_noise_suppression(engine,3)==0 && mv_set_enhanced_suppression(engine,3)==0,"Configuration failed");
    mv_stats(engine,&stats); require(std::abs(stats.suppression_delay_ms-(60+334./48))<1e-8,"Combined stats delay incorrect");
    mv_destroy(engine);
    std::sort(times.begin(),times.end());
    std::cout<<"Room frame ms: p50 "<<times[times.size()/2]<<", p99 "<<times[size_t(times.size()*.99)]<<", max "<<times.back()<<"\n";
    std::cout<<"PASS reference, zero streaming allocations, exact reset, startup alignment, malformed input, 120 s stability, all 20 chains and delay accounting\n";
} catch(const std::exception &e) { std::cerr<<"FAIL: "<<e.what()<<"\n";return 1; } }
