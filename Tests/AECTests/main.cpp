#include "mv_webrtc_aec.h"
#include "EchoReferenceQueue.hpp"
#include "mv_audio.h"
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>
#include <limits>
#include <thread>
using AEC = std::unique_ptr<MVEchoCanceller, decltype(&mv_aec_destroy)>;
static void require(bool ok,const char *message) {if(!ok)throw std::runtime_error(message);}
static AEC create(){AEC a(mv_aec_create(),mv_aec_destroy);require(bool(a),"AEC creation");return a;}
static void queueTests() {
    constexpr double start=1e12, step=1e9/48000.;
    std::array<float,8192> l{},r{};std::array<float,480> ol{},orr{};
    for(size_t i=0;i<l.size();++i){l[i]=float(i)*.0001f;r[i]=-l[i];}
    auto q=std::make_unique<mv::EchoReferenceQueue>();
    require(q->readBlock(start,ol.data(),orr.data())==0,"Empty reference must wait");
    require(q->push(l.data(),r.data(),480,1,start,48000),"Push stereo reference");
    require(q->readBlock(start-step*5,ol.data(),orr.data())==-1,"Too-old microphone must be rejected");
    require(q->readBlock(start,ol.data(),orr.data())==1,"Exact full block must not require extra future sample");
    for(size_t i=0;i<480;++i)require(std::abs(ol[i]-l[i])<1e-7 && orr[i]==-ol[i],"Stereo anti-phase must not cancel in reference");
    require(q->readBlock(start+480*step,ol.data(),orr.data())==0,"Future reference must wait");
    require(q->push(l.data(),r.data(),480,1,start+500*step,48000),"Push after gap");
    require(q->readBlock(start+480*step,ol.data(),orr.data())==-1,"Timestamp hole must be rejected");
    require(!q->push(l.data(),r.data(),480,1,start,48000),"Out-of-order timestamps rejected");
    q=std::make_unique<mv::EchoReferenceQueue>();
    for(int i=0;i<8;++i)require(q->push(l.data(),r.data(),8192,1,start+i*8192*step,48000),"Fill bounded queue");
    require(!q->push(l.data(),r.data(),1,1,start+65536*step,48000) && q->overruns()==1,"Overflow detected");
    q->discard();require(q->push(l.data(),r.data(),480,1,start+65536*step,48000),"Discard frees queue");
    l[5]=std::numeric_limits<float>::quiet_NaN();
    require(!q->push(l.data(),r.data(),480,1,start+66016*step,48000),"NaN reference rejected atomically");l[5]=.0005f;
    for(double rate : {44100.,48000.*.9998,48000.,48000.*1.0002,96000.}) {
        q=std::make_unique<mv::EchoReferenceQueue>();
        const double rs=1e9/rate;double next=start+rs*.4;
        for(int b=0;b<400;++b) {
            for(int i=0;i<128;++i){const double t=(b*128+i)*rs/1e9;l[i]=float(std::sin(2*M_PI*321*t));r[i]=-l[i];}
            require(q->push(l.data(),r.data(),128,1,start+b*128*rs,rate),"Clock drift producer");
            while(q->readBlock(next,ol.data(),orr.data())==1) {
                for(int i=0;i<480;++i)require(std::abs(ol[i]-std::sin(2*M_PI*321*(next-start+i*step)/1e9))<.0004 && ol[i]==-orr[i],"Timestamp interpolation / wrap / drift");
                next+=480*step;
            }
        }
    }
    // Concurrent producer/consumer wraps storage many times without dropping.
    q=std::make_unique<mv::EchoReferenceQueue>();std::atomic<int> consumed{0};
    std::thread producer([&]{std::array<float,480>a{},b{};for(int block=0;block<1000;++block){
        while(block-consumed.load()>80)std::this_thread::yield();
        a.fill(float(block));b.fill(float(-block));
        require(q->push(a.data(),b.data(),480,1,start+block*480*step,48000),"Concurrent push");
    }});
    for(int block=0;block<1000;++block){int result;while((result=q->readBlock(start+block*480*step,ol.data(),orr.data()))==0)std::this_thread::yield();
        require(result==1,"Concurrent read");for(int i=0;i<480;++i)require(ol[i]==block && orr[i]==-block,"Concurrent sample ownership");consumed=block+1;}
    producer.join();std::cout<<"PASS reference timing, stereo, drift, gaps, bounds and concurrent wrap\n";
}
static void impulseTests(){
    for(int position:{0,37,159,320,479}) {
        auto a=create();std::array<float,480> l{},r{},mic{};std::vector<float> out;
        for(int b=0;b<20;++b){mic.fill(0);if(b==10)mic[position]=.1f;require(mv_aec_process(a.get(),l.data(),r.data(),mic.data())==0,"Impulse process");out.insert(out.end(),mic.begin(),mic.end());}
        const auto peak=std::max_element(out.begin(),out.end(),[](float a,float b){return std::abs(a)<std::abs(b);})-out.begin();
        require(peak-4800-position==mv_aec_delay_frames(),"AEC algorithm delay mismatch");
        require(mv_aec_reset(a.get())==0,"AEC reset");
        mic.fill(0);mic[0]=std::numeric_limits<float>::infinity();require(mv_aec_process(a.get(),l.data(),r.data(),mic.data())!=0,"Nonfinite mic rejected");
        for(auto value:mic)require(value==0,"Fail closed on malformed input");
    }
    std::cout<<"PASS AEC impulse delay "<<mv_aec_delay_frames()<<" samples ("<<mv_aec_delay_frames()/48.<<" ms), reset and malformed input\n";
}
static double db(double a,double b){return 10*std::log10((a+1e-20)/(b+1e-20));}
static void qualityTest(bool stereo,bool nearOnly){
    constexpr size_t n=48000*24;const size_t delay=mv_aec_delay_frames();
    std::vector<float> l(n),r(n),near(n),mic(n),out(n);std::vector<double> times;times.reserve(n/480);
    std::mt19937 gen(8397);std::uniform_real_distribution<float> noise(-1,1);
    double fl=0,fr=0,fn=0;
    for(size_t i=0;i<n;++i){
        fl=.7*fl+.3*noise(gen);fr=.7*fr+.3*noise(gen);fn=.6*fn+.4*noise(gen);
        double t=i/48000.;
        l[i]=nearOnly?0:float(.28*fl);r[i]=stereo?float(.23*fr):l[i];
        // Independent voiced harmonics and broadband consonant-like component.
        double gate=.3+.7*std::pow(std::sin(M_PI*2.7*t),2);
        near[i]=(nearOnly || t>=14)?float(gate*(.09*std::sin(2*M_PI*173*t)+.04*std::sin(2*M_PI*346*t)+.07*fn)):0;
        size_t d=t<8?1440:2400; // Abrupt echo-path change from 30 to 50 ms.
        mic[i]=near[i]+(i>=d?.5f*l[i-d]+.3f*r[i-d]:0)+(i>=d+371?.12f*l[i-d-371]-.08f*r[i-d-371]:0);
    }
    auto a=create();
    for(size_t i=0;i<n;i+=480){std::copy_n(mic.data()+i,480,out.data()+i);const auto before=std::chrono::steady_clock::now();
        require(mv_aec_process(a.get(),l.data()+i,r.data()+i,out.data()+i)==0,"Quality process");
        times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count());}
    double inEcho=0,outEcho=0,xy=0,xx=0,yy=0,err=0;
    for(size_t i=48000*10;i<48000*13;++i){inEcho+=double(mic[i])*mic[i];outEcho+=double(out[i+delay])*out[i+delay];}
    for(size_t i=48000*18;i<n-delay;++i){const double x=near[i],y=out[i+delay];xy+=x*y;xx+=x*x;yy+=y*y;err+=(x-y)*(x-y);}
    double reduction=db(inEcho,outEcho),gain=xy/xx,correlation=xy/std::sqrt(xx*yy),snr=db(xx,err);
    std::sort(times.begin(),times.end());
    std::cout<<(nearOnly?"Near voice / silent playback":stereo?"Stereo echo + double talk":"Mono echo + double talk")
        <<": echo reduction "<<reduction<<" dB, near gain "<<gain<<", correlation "<<correlation<<", SNR "<<snr
        <<" dB; processing p50 "<<times[times.size()/2]<<", p99 "<<times[size_t(times.size()*.99)]<<" ms\n";
    require(nearOnly || reduction>15,"Insufficient far-end echo cancellation after path change");
    require(gain>.65 && gain<1.15 && correlation>.85,"Near voice damaged during double talk / silent reference");
}
int main(){try{
    queueTests();impulseTests();qualityTest(false,true);qualityTest(false,false);qualityTest(true,false);
    auto *e=mv_create();require(mv_set_echo_cancellation(e,1)==0 && mv_set_echo_cancellation(e,2)!=0,"Echo settings validation");
    MVStats stats{};mv_stats(e,&stats);require(stats.echo_cancellation==1 && std::abs(stats.echo_delay_ms-430./48)<1e-9,"Echo stats");mv_destroy(e);
    std::cout<<"PASS all offline Echo tests (no audio devices opened)\n";return 0;
}catch(const std::exception &e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
