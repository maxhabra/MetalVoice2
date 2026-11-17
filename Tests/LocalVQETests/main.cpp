#include "LocalVQEAdapter.hpp"
#include "mv_localvqe.h"
#include "localvqe_api.h"
#include "NoisePipeline.hpp"
#include "mv_audio.h"
#include "mv_webrtc_ns.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <sys/resource.h>
#include <time.h>
#include <vector>

using VQE = std::unique_ptr<MVLocalVQE, decltype(&mv_localvqe_destroy)>;
static void require(bool yes, const char *message) { if (!yes) throw std::runtime_error(message); }
static bool identity(void *, const float *input, float *output) { std::copy_n(input, 256, output); return true; }
static double cpuTime() { timespec t{}; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t); return t.tv_sec+t.tv_nsec/1e9; }
static std::vector<float> signal(size_t blocks) {
    std::vector<float> x(blocks*480);
    for (size_t i=0;i<x.size();++i) {
        double t=i/48000.;
        double envelope=.25+.75*std::pow(std::sin(2*M_PI*2.7*t),2);
        x[i]=float(envelope*(.15*std::sin(2*M_PI*173*t)+.08*std::sin(2*M_PI*346*t)+.04*std::sin(2*M_PI*1103*t)));
    }
    return x;
}
static void adapterTests() {
    constexpr size_t delay=mv::LocalVQEAdapter::framingDelay+mv::LocalVQEAdapter::filterDelay;
    // Cover every 10/16 ms scheduling phase and all three resampling phases.
    for (size_t b=0;b<8;++b) for (size_t phase=0;phase<3;++phase) {
        mv::LocalVQEAdapter adapter;
        std::vector<float> x(480*16), y(x.size());
        const size_t position=b*480+phase+57;
        x[position]=.5f;
        for (size_t i=0;i<x.size();i+=480) require(adapter.process(x.data()+i,y.data()+i,identity,nullptr),"Adapter FIFO underrun/overflow");
        const size_t peak=std::max_element(y.begin(),y.end(),[](float a,float b){return std::abs(a)<std::abs(b);})-y.begin();
        require(peak==position+delay,"Resampler/framing alignment is not constant");
        adapter.reset(); auto again=x;
        for (size_t i=0;i<x.size();i+=480) require(adapter.process(again.data()+i,again.data()+i,identity,nullptr),"Aliased adapter process");
        require(again==y,"Adapter reset/aliasing differs");
    }
    for (double hz : {0.,100.,1000.,6000.,8000.,9000.,15000.,21000.}) {
        mv::LocalVQEAdapter adapter;
        std::vector<float> x(48000), y(x.size());
        for (size_t i=0;i<x.size();++i) x[i]=float(.2*std::cos(2*M_PI*hz*i/48000.+.37));
        for (size_t i=0;i<x.size();i+=480) require(adapter.process(x.data()+i,y.data()+i,identity,nullptr),"Tone resample");
        double xx=0,yy=0,error=0;
        for (size_t i=4800;i<x.size()-delay;++i) {
            xx+=x[i]*x[i]; yy+=y[i+delay]*y[i+delay]; error+=std::pow(y[i+delay]-x[i],2);
        }
        const double gain=std::sqrt(yy/xx);
        std::printf("Resampler %.0f Hz gain %.2f dB\n",hz,20*std::log10(gain+1e-30));
        if (hz<=6000) require(gain>.99 && gain<1.01 && std::sqrt(error/xx)<.01,"Passband gain/phase changed");
        else require(gain<.001,"Aliasing attenuation below 60 dB");
    }
    // Long continuous framing run: bounded storage must survive many wraps.
    mv::LocalVQEAdapter adapter; std::array<float,480> block{};
    for (int i=0;i<60000;++i) require(adapter.process(block.data(),block.data(),identity,nullptr),"Ten-minute adapter soak");
    std::puts("PASS adapter timing, passband, anti-aliasing, reset and 10-minute FIFO soak");
}
static void engineTests() {
    auto *e=mv_create(); require(e,"Engine allocation");
    for (bool modeFirst : {false,true}) {
        mv_set_enhanced_suppression(e,0);
        if (modeFirst) require(mv_set_enhanced_suppression(e,4)==0,"LocalVQE mode rejected");
        require(mv_set_echo_cancellation(e,1)==0 && mv_set_room_dereverb(e,1)==0,"Saved room settings rejected");
        if (!modeFirst) require(mv_set_enhanced_suppression(e,4)==0,"LocalVQE mode rejected");
        MVStats s{};mv_stats(e,&s);
        require(s.echo_cancellation==0 && s.room_dereverb==0 && s.echo_delay_ms==0 && s.room_dereverb_delay_ms==0,"LocalVQE incompatible stages still enabled");
        require(s.suppression_delay_ms==34 && s.localvqe_delay_ms==34,"LocalVQE delay report");
        require(mv_set_noise_suppression(e,3)==0,"WebRTC setting");mv_stats(e,&s);
        require(std::abs(s.suppression_delay_ms-(1632+334)/48.)<1e-9,"WebRTC + LocalVQE delay");
        mv_set_noise_suppression(e,-1);
        mv_set_enhanced_suppression(e,1);mv_stats(e,&s);
        require(s.echo_cancellation==1 && s.room_dereverb==1 && s.localvqe_delay_ms==0,"Previous Echo/WPE preferences not restored");
    }
    require(mv_set_enhanced_suppression(e,5)!=0,"Invalid mode accepted");mv_destroy(e);
    std::puts("PASS native incompatibility enforcement and restoration, both setter orders");
}
static std::vector<float> run(MVLocalVQE *vqe, const std::vector<float> &input, const char *label, bool benchmark=false) {
    auto output=input; std::vector<double> times; double cpu=cpuTime();
    for (size_t i=0;i<input.size();i+=480) {
        auto before=std::chrono::steady_clock::now();
        require(mv_localvqe_process(vqe,output.data()+i,output.data()+i)==0,"LocalVQE processing failed");
        times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count());
        for (size_t j=i;j<i+480;++j) require(std::isfinite(output[j]) && std::abs(output[j])<=1,"Nonfinite/unbounded output");
    }
    const double cpuPercent=(cpuTime()-cpu)/(input.size()/48000.)*100;
    if (benchmark) {
        double sum=0;for (auto ms:times)sum+=ms;
        std::sort(times.begin(),times.end());
        std::printf("%s mean %.3f, p99 %.3f, max %.3f ms / 10 ms block; CPU %.1f%% of one core\n",label,sum/times.size(),times[size_t(times.size()*.99)],times.back(),cpuPercent);
    }
    return output;
}
static void modelTests(const char *path) {
    char message[256]{};
    require(!mv_localvqe_create("/missing/model",0,message,sizeof(message)) && message[0],"Missing model accepted");
    require(!mv_localvqe_create(path,5,nullptr,0),"Invalid thread count accepted");
    VQE vqe(mv_localvqe_create(path,0,message,sizeof(message)),mv_localvqe_destroy);
    require(bool(vqe),message);require(mv_localvqe_threads(vqe.get())==2,"Default worker count");
    const auto input=signal(400);
    auto first=run(vqe.get(),input,"LocalVQE",true);
    require(mv_localvqe_reset(vqe.get())==0,"Reset failed");
    require(run(vqe.get(),input,"reset")==first,"Reset leaked prior stream state");
    // Verify model waveform alignment, independently of the adapter impulse test.
    double best=-1;int bestDelay=-1;
    for (int d=1500;d<1750;++d) {
        double xy=0,xx=0,yy=0;
        for (size_t i=48000;i<input.size()-1750;++i) {xy+=input[i]*first[i+d];xx+=input[i]*input[i];yy+=first[i+d]*first[i+d];}
        const double corr=xy/std::sqrt(xx*yy+1e-30);
        if (corr>best) {best=corr;bestDelay=d;}
    }
    std::printf("Model correlation peak delay %d samples (reported %u), correlation %.4f\n",bestDelay,mv_localvqe_delay_frames(),best);
    require(std::abs(bestDelay-int(mv_localvqe_delay_frames()))<=2 && best>.85,"Model temporal alignment mismatch");
    for (int level=-1;level<=3;++level) {
        mv::NoisePipeline pipeline;
        require(pipeline.configure(level,4,false,path,0,nullptr,true),"Pipeline configure");
        require(pipeline.localVQEThreads()==2 && mv::NoisePipeline::delayFrames(level,4,true)==mv_localvqe_delay_frames()+(level>=0?334:0),"WPE must be excluded from LocalVQE chain/delay");
        require(mv_localvqe_reset(vqe.get())==0,"Reference reset");
        std::unique_ptr<MVNoiseSuppressor,decltype(&mv_ns_destroy)> ns(level<0?nullptr:mv_ns_create(level),mv_ns_destroy);
        for (size_t i=0;i<480*40;i+=480) {
            std::array<float,480>a{},b{};std::copy_n(input.data()+i,480,a.data());b=a;
            if (ns) mv_ns_process(ns.get(),b.data());
            for(auto &sample:b)sample=std::clamp(sample,-1.f,1.f);
            require(mv_localvqe_process(vqe.get(),b.data(),b.data())==0 && pipeline.process(a.data()),"Cascade process");
            for(size_t j=0;j<480;++j)require(std::abs(a[j]-b[j])<1e-6,"Cascade differs / WPE unexpectedly applied");
        }
        require(pipeline.configure(-1,0) && pipeline.localVQEThreads()==0,"Bypass did not release model");
        std::printf("PASS WebRTC level %d + LocalVQE, WPE exclusion, release\n",level);
    }
    std::array<float,480>bad{},out{};
    bad[7]=std::numeric_limits<float>::quiet_NaN();
    require(mv_localvqe_process(vqe.get(),bad.data(),out.data())!=0,"Nonfinite audio accepted");
    require(std::all_of(out.begin(),out.end(),[](float v){return v==0;}),"Error output not silent");
    require(mv_localvqe_process(nullptr,out.data(),out.data())!=0 && mv_localvqe_reset(nullptr)!=0,"Null state guard");
    require(run(vqe.get(),input,"after error")==first,"Failure did not clear streaming history");
    require(mv_localvqe_reset(vqe.get())==0,"Silence reset");
    auto silent=run(vqe.get(),std::vector<float>(480*80),"silence");
    require(std::all_of(silent.begin(),silent.end(),[](float x){return std::abs(x)<1e-6;}),"Silence produces audible output");
    std::puts("PASS model reset, alignment, all cascades, malformed input and silence");
}
int main(int argc,char **argv) {
    std::setvbuf(stdout,nullptr,_IOLBF,0);
    try {
        require(argc==2,"Usage: localvqe-tests MODEL.gguf");
        adapterTests();engineTests();modelTests(argv[1]);
        rusage usage{};getrusage(RUSAGE_SELF,&usage);
        std::printf("Test process peak resident %.1f MiB\n",usage.ru_maxrss/1048576.);
        std::puts("PASS LocalVQE tests (no audio devices opened)");return 0;
    } catch (const std::exception &e) { std::fprintf(stderr,"FAIL LocalVQE: %s\n",e.what());return 1; }
}
