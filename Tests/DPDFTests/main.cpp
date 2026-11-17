#include "NoisePipeline.hpp"
#include "mv_audio.h"
#include "mv_dpdf.h"
#include "mv_webrtc_ns.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <sys/resource.h>
#include <time.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
std::vector<float> read(const char *path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    require(bool(f), "Missing reference fixture");
    const auto size = size_t(f.tellg());
    require(size > 0 && size % (480*sizeof(float)) == 0, "Invalid fixture size");
    std::vector<float> result(size / sizeof(float));
    f.seekg(0); f.read(reinterpret_cast<char *>(result.data()), size);
    require(bool(f), "Could not read fixture");
    return result;
}
double cpuTime() {
    timespec time{}; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time);
    return time.tv_sec + time.tv_nsec/1e9;
}
std::vector<float> process(mv::NoisePipeline &pipeline, const std::vector<float> &input, const char *label) {
    auto output = input;
    std::vector<double> times;
    double cpu = cpuTime();
    for (size_t i=0; i<input.size(); i+=480) {
        const auto start = Clock::now();
        require(pipeline.process(output.data()+i), "Pipeline processing failed");
        times.push_back(std::chrono::duration<double,std::milli>(Clock::now()-start).count());
        for (size_t j=i; j<i+480; ++j) require(std::isfinite(output[j]) && std::abs(output[j]) <= 1, "Unsafe output");
    }
    cpu = (cpuTime()-cpu)/(input.size()/48000.)*100;
    std::sort(times.begin(),times.end());
    std::printf("%s p50=%.4f p99=%.4f max=%.4f ms; offline CPU=%.3f%% of one core\n",
        label,times[times.size()/2],times[size_t(times.size()*.99)],times.back(),cpu);
    return output;
}
void compare(const std::vector<float> &actual, const std::vector<float> &reference) {
    require(actual.size()==reference.size(), "Reference length mismatch");
    double error=0, energy=0, maximum=0;
    for (size_t i=0; i<actual.size(); ++i) {
        const double expected=std::clamp(reference[i],-1.f,1.f), difference=actual[i]-expected;
        error+=difference*difference; energy+=expected*expected; maximum=std::max(maximum,std::abs(difference));
    }
    double relative=std::sqrt(error/std::max(energy,1e-20));
    std::printf("ONNX audio parity max=%.9g relative_RMS=%.9g\n",maximum,relative);
    require(maximum<1e-4 && relative<1e-4, "Official ONNX audio mismatch");
}
}

int main(int argc,char **argv) {
    std::setvbuf(stdout,nullptr,_IOLBF,0);
    try {
        require(argc==6, "Usage: dpdf-tests MODEL.mlmodelc STATE.f32 INPUT.f32 REFERENCE.f32 DFN3.mlmodelc");
        const auto input=read(argv[3]), expected=read(argv[4]);
        auto *engine=mv_create();
        require(engine && mv_set_enhanced_suppression(engine,3)==0 && mv_set_noise_suppression(engine,1)==0, "DPDF engine configuration");
        MVStats stats{}; mv_stats(engine,&stats);
        require(stats.enhanced_suppression==3 && stats.dpdf_delay_ms==50 && stats.deepfilter_delay_ms==0 && stats.rnnoise_delay_ms==0, "DPDF delay report");
        require(std::abs(stats.suppression_delay_ms-2734./48)<1e-9, "Combined delay report");
        require(mv_set_enhanced_suppression(engine,5)!=0, "Invalid mode accepted");
        mv_destroy(engine);
        mv::NoisePipeline pipeline;
        require(!pipeline.configure(-1,3,true,"/missing/model",0,argv[2]), "Missing model accepted");
        require(!pipeline.configure(-1,3,true,argv[1],0,"/missing/state"), "Missing state accepted");
        require(pipeline.configure(-1,3,true,argv[1],0,argv[2]), "DPDF init failed");
        std::printf("DPDF preferred compute devices=%u\n",pipeline.dpdfComputeDevices());
        auto first=process(pipeline,input,"DPDFNet-2"); compare(first,expected);
        require(pipeline.reset(), "DPDF reset failed");
        auto repeated=process(pipeline,input,"DPDFNet-2 reset");
        require(first==repeated, "Reset leaked prior stream state");
        for (int level=0; level<=3; ++level) {
            require(pipeline.configure(level,3,true,argv[1],0,argv[2]), "Combined init failed");
            std::unique_ptr<MVDPDF,decltype(&mv_dpdf_destroy)> ref(mv_dpdf_create(argv[1],argv[2],0,nullptr,0),mv_dpdf_destroy);
            std::unique_ptr<MVNoiseSuppressor,decltype(&mv_ns_destroy)> ns(mv_ns_create(level),mv_ns_destroy);
            require(ref && ns, "Combined reference init failed");
            for (size_t i=0; i<std::min(input.size(),size_t(480*240)); i+=480) {
                std::array<float,480> a{},b{};
                std::copy_n(input.data()+i,480,a.data()); b=a;
                mv_ns_process(ns.get(),b.data());
                for (auto &v:b) v=std::clamp(v,-1.f,1.f);
                require(mv_dpdf_process(ref.get(),b.data(),b.data())==0 && pipeline.process(a.data()),"Combined processing failed");
                for (size_t j=0;j<480;++j) require(std::abs(a[j]-std::clamp(b[j],-1.f,1.f))<1e-6,"WebRTC cascade mismatch");
            }
            std::printf("WebRTC level %d cascade passed\n",level);
        }
        require(pipeline.configure(-1,1), "RNNoise switch failed");
        process(pipeline,input,"RNNoise");
        require(pipeline.configure(-1,2,true,argv[5]), "DFN3 switch failed");
        process(pipeline,input,"DeepFilterNet3 Core ML");
        require(pipeline.configure(-1,3,true,argv[1],0,argv[2]), "Return to DPDF failed");
        compare(process(pipeline,input,"DPDFNet-2 after switch"),expected);
        std::array<float,480> malformed{}, output{};
        malformed[5]=std::numeric_limits<float>::infinity();
        malformed[6]=std::numeric_limits<float>::quiet_NaN();
        require(pipeline.process(malformed.data()), "Pipeline should sanitize malformed capture");
        for (float v:malformed) require(std::isfinite(v),"Sanitization failed");
        std::unique_ptr<MVDPDF,decltype(&mv_dpdf_destroy)> direct(mv_dpdf_create(argv[1],argv[2],0,nullptr,0),mv_dpdf_destroy);
        require(bool(direct),"Direct init");
        malformed[5]=std::numeric_limits<float>::infinity();
        require(mv_dpdf_process(direct.get(),malformed.data(),output.data())!=0,"Raw API accepted infinity");
        require(std::all_of(output.begin(),output.end(),[](float x){return x==0;}),"Error output not silent");
        require(mv_dpdf_process(nullptr,output.data(),output.data())!=0 && mv_dpdf_reset(nullptr)!=0,"Null API guard");
        require(pipeline.configure(-1,0) && pipeline.dpdfComputeDevices()==0,"DPDF state not released on bypass");
        rusage usage{}; getrusage(RUSAGE_SELF,&usage);
        std::printf("Test process peak resident=%.2f MiB\n",usage.ru_maxrss/1048576.);
        std::puts("DPDF tests passed"); return 0;
    } catch(const std::exception &e) { std::fprintf(stderr,"DPDF tests failed: %s\n",e.what()); return 1; }
}
