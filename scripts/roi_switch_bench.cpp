// ROI movement benchmark. Requires Magewell easycap and GenTL producer.
#include "session.hpp"
#include "CEasyCapSEx.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Clock=std::chrono::steady_clock;
static std::atomic<bool> interrupted{false};
static void on_signal(int) { interrupted=true; }
static constexpr int W=7008,H=1750;
static const auto origin=Clock::now();
static double now_us() { return std::chrono::duration<double,std::micro>(Clock::now()-origin).count(); }
struct Frame {
    std::string test, mode;
    int group=0, block=0, index=0, expected=-1, observed=-1;
    uint64_t id=0, ts=0;
    double ready=0, copy=0, score=0, margin=0;
};
struct Switch {
    std::string test, mode;
    int group=0, block=0, target=0, issue_frame=0;
    double begin=0, after_stop=0, after_write=0, after_start=0, first=-1, previous=-1;
    int stale=0;
};
struct ReturnedBuffer {
    Session& session; BUFFER_HANDLE handle=nullptr;
    explicit ReturnedBuffer(Session& s):session(s) {}
    ~ReturnedBuffer() { if(handle) scap_back_buffer(session.cap,handle); }
};
static std::vector<double> profile(const unsigned char* p,int y0) {
    std::vector<double> v;
    v.reserve(2048);
    for(int j=0;j<32;++j) for(int i=0;i<64;++i) {
        int x=32+i*(W-64)/64, y=y0+16+j*(H-32)/32;
        double sum=0;
        for(int dy=0;dy<8;++dy) for(int dx=0;dx<8;++dx) sum+=p[(y+dy)*W+x+dx];
        v.push_back(sum/64.);
    }
    return v;
}
static double normalize(std::vector<double>& v) {
    double mean=std::accumulate(v.begin(),v.end(),0.)/v.size(), ss=0;
    for(double& x:v) { x-=mean; ss+=x*x; }
    double norm=std::sqrt(ss);
    if(norm>1e-9) for(double& x:v) x/=norm;
    return norm;
}
struct Rig {
    Session s;
    std::map<std::string,std::string> saved;
    bool snapshot=false, restored=false;
    std::array<std::vector<double>,4> refs;
    std::vector<unsigned char> scratch=std::vector<unsigned char>(W*H);
    std::ofstream frames, switches, status;
    std::filesystem::path out;
    int cycles;
    std::string write_node;
    int minimum_pipeline_frames=512;
    std::vector<Frame> batch;
    std::vector<Switch> changes;
    explicit Rig(const std::filesystem::path& dir,int n,const std::string& node):out(dir),cycles(n),write_node(node) {
        std::filesystem::create_directory(out);
        frames.open(out/"frames.csv"); switches.open(out/"switches.csv"); status.open(out/"status.log");
        frames<<"test,mode,group,block,index,expected,observed,frame_id,device_timestamp,ready_us,copy_us,score,margin\n";
        switches<<"test,mode,group,block,target,issue_frame,begin_us,after_stop_us,after_write_us,after_start_us,first_target_us,previous_frame_us,stale_frames\n";
        frames<<std::fixed<<std::setprecision(6); switches<<std::fixed<<std::setprecision(6);
    }
    void check(bool ok,const std::string& what) { if(!ok) throw std::runtime_error(what+": "+last_err()); }
    void set(const char* key,const std::string& value,bool verify=true) {
        check(set_feature(s.cap,scap_camera,key,value.c_str()),std::string("set ")+key);
        if(verify) { std::string actual; check(get_feature(s.cap,scap_camera,key,actual),std::string("get ")+key);
            if(actual!=value) throw std::runtime_error(std::string(key)+" requested="+value+" actual="+actual);
        }
    }
    void initialize() {
        Opt opt; check(s.open(opt,OpenMode::Camera),"open");
        for(auto key:{"Width","Height","OffsetX","OffsetY","PixelFormat","ExposureTime","TriggerMode","AcquisitionMode","AcquisitionFrameRateEnable","TLParamsLocked"})
            check(get_feature(s.cap,scap_camera,key,saved[key]),std::string("snapshot ")+key);
        snapshot=true;
        status<<"write_node="<<write_node<<'\n';
        status<<"controller_policy=skip_missed_frame_thresholds\n";
        if(write_node!="OffsetY" && s.model!="MV-AX5F57MP050E") throw std::runtime_error("OffsetYReg experiment is specific to MV-AX5F57MP050E");
        std::ofstream initial(out/"initial.csv");
        initial<<"feature,value\n"; for(const auto& kv:saved) initial<<kv.first<<','<<kv.second<<'\n';
        for(auto key:{"SequencerMode","MultipleROIEnable"}) {
            std::string v; if(get_feature(s.cap,scap_camera,key,v)) {
                status<<key<<'='<<v<<'\n';
                if(v!="Off" && v!="0") throw std::runtime_error(std::string(key)+" must be disabled before this experiment");
            }
        }
        set("AcquisitionFrameRateEnable","0"); set("TriggerMode","Off"); set("AcquisitionMode","Continuous");
        set("OffsetX","0"); set("OffsetY","0"); set("PixelFormat","Mono8");
        set("Width",std::to_string(W)); set("Height",std::to_string(H)); set("ExposureTime","300");
    }
    bool write_y(int value) {
        // Constant payload; only the four previously validated vertical positions.
        if(value<0 || value>5250 || value%H!=0) throw std::runtime_error("invalid offset");
        if(write_node=="port") {
            // Address and endian verified against this camera's downloaded GenICam XML:
            // ImageFormatControlBase=0x30010000, OffsetYReg offset=0x002C, 4-byte BE.
            unsigned char bytes[4]={0,0,static_cast<unsigned char>(value>>8),static_cast<unsigned char>(value)};
            size_t n=sizeof(bytes);
            return scap_write_port(s.cap,scap_camera,0x3001002c,bytes,&n) && n==sizeof(bytes);
        }
        return scap_set_integer(s.cap,scap_camera,write_node.c_str(),value);
    }
    Frame capture(const std::string& test,const std::string& mode,int group,int block,int index,int expected) {
        if(interrupted) throw std::runtime_error("interrupted");
        ReturnedBuffer returned(s); void* data=nullptr; size_t size=0;
        check(scap_get_buf(s.cap,1000,&returned.handle,&data,&size),"get buffer");
        if(!data || size!=size_t(W*H)) throw std::runtime_error("unexpected payload");
        Frame f; f.test=test; f.mode=mode; f.group=group; f.block=block; f.index=index; f.expected=expected; f.ready=now_us();
        cxp_buf_info_t info{}; check(scap_get_buf_info(s.cap,returned.handle,&info,true),"buffer metadata");
        f.id=info.frame_id; f.ts=info.timestamp;
        if(info.width!=W || info.height!=H) throw std::runtime_error("wrong frame dimensions");
        if(mode=="copy") { double a=now_us(); std::memcpy(scratch.data(),data,size);
            asm volatile("" :: "r"(scratch.data()):"memory"); f.copy=now_us()-a; }
        auto v=profile(static_cast<unsigned char*>(data),0); normalize(v);
        std::array<double,4> scores{};
        for(int k=0;k<4;++k) scores[k]=std::inner_product(v.begin(),v.end(),refs[k].begin(),0.);
        int best=std::max_element(scores.begin(),scores.end())-scores.begin();
        f.score=scores[best]; auto sorted=scores; std::sort(sorted.begin(),sorted.end()); f.margin=sorted[3]-sorted[2];
        if(f.score>=.90 && f.margin>=.002) f.observed=best;
        batch.push_back(f);
        return f;
    }
    void reference() {
        set("Height","7000"); check(s.start(8),"reference start");
        for(int i=0;i<8;++i) {
            ReturnedBuffer returned(s); void* data=nullptr;size_t size=0;
            check(scap_get_buf(s.cap,1000,&returned.handle,&data,&size),"reference pull");
            if(size!=size_t(W*7000)) throw std::runtime_error("reference payload");
            if(i==7) {
                std::ofstream preview(out/"reference.pgm",std::ios::binary); preview<<"P5\n64 128\n255\n";
                for(int k=0;k<4;++k) {
                    refs[k]=profile(static_cast<unsigned char*>(data),k*H);
                    for(double value:refs[k]) { unsigned char c=std::clamp(value,0.,255.); preview.write(reinterpret_cast<char*>(&c),1); }
                    double norm=normalize(refs[k]); status<<"reference_norm_"<<k<<'='<<norm<<'\n';
                    if(norm<.1) throw std::runtime_error("reference has insufficient image contrast");
                }
            }
        }
        s.stop(); set("Height",std::to_string(H));
    }
    void flush() {
        for(const auto& f:batch) frames<<f.test<<','<<f.mode<<','<<f.group<<','<<f.block<<','<<f.index<<','<<f.expected<<','<<f.observed<<','<<f.id<<','<<f.ts<<','<<f.ready<<','<<f.copy<<','<<f.score<<','<<f.margin<<'\n';
        for(const auto& x:changes) switches<<x.test<<','<<x.mode<<','<<x.group<<','<<x.block<<','<<x.target<<','<<x.issue_frame<<','<<x.begin<<','<<x.after_stop<<','<<x.after_write<<','<<x.after_start<<','<<x.first<<','<<x.previous<<','<<x.stale<<'\n';
        batch.clear(); changes.clear(); frames.flush(); switches.flush(); status.flush();
    }
    void prepare(const std::string& mode,bool unlock) {
        s.stop(); set("OffsetY","0"); check(s.start(8),"start");
        for(int i=0;i<16;++i) capture("warmup",mode,0,0,i,0);
        if(unlock) check(scap_set_integer(s.cap,scap_camera,"TLParamsLocked",0),"unlock TLParamsLocked");
        flush();
    }
    void baseline() {
        for(const auto& mode:{std::string("discard"),std::string("copy")}) for(int zone=0;zone<4;++zone) {
            s.stop(); set("OffsetY",std::to_string(zone*H)); check(s.start(8),"baseline start");
            for(int i=0;i<16;++i) capture("warmup",mode,0,zone,i,zone);
            for(int i=0;i<96;++i) capture("baseline",mode,0,zone,i,zone);
            flush();
        }
        s.stop();
    }
    void full_split() {
        s.stop();set("OffsetY","0");set("Height","7000");
        std::vector<unsigned char> split(7000*7000);
        for(const auto& mode:{std::string("discard"),std::string("split_copy")}) {
            check(s.start(8),"full height start");
            for(int i=0;i<528;++i) {
                if(interrupted) throw std::runtime_error("interrupted");
                ReturnedBuffer returned(s);void* data=nullptr;size_t size=0;
                check(scap_get_buf(s.cap,1000,&returned.handle,&data,&size),"full height pull");
                if(size!=size_t(W*7000)) throw std::runtime_error("full height payload");
                Frame f;f.test=i<16?"warmup":"full_frame";f.mode=mode;f.index=i-16;f.ready=now_us();
                cxp_buf_info_t info{};check(scap_get_buf_info(s.cap,returned.handle,&info,true),"full frame metadata");
                f.id=info.frame_id;f.ts=info.timestamp;
                if(mode=="split_copy") {
                    double a=now_us();
                    // Four contiguous packed 7000x1750 images, discarding 8 padding columns.
                    for(int y=0;y<7000;++y) std::memcpy(split.data()+size_t(y)*7000,static_cast<unsigned char*>(data)+size_t(y)*W,7000);
                    asm volatile("" :: "r"(split.data()):"memory");f.copy=now_us()-a;
                }
                batch.push_back(f);
            }
            s.stop();flush();
            std::printf("DONE full_frame %s\n",mode.c_str());std::fflush(stdout);
        }
    }
    void write_only() {
        s.stop();
        for(int i=0;i<64;++i) {
            Switch x; x.test="write_only";x.mode="none";x.block=i;x.target=i%4;
            x.begin=x.after_stop=now_us();
            check(write_y(x.target*H),"stopped write OffsetY");
            x.after_write=x.after_start=now_us(); changes.push_back(x);
        }
        flush();
    }
    void serial(const std::string& test,const std::string& mode,int group) {
        prepare(mode,test=="live_confirmed");
        Frame previous=capture("warmup",mode,0,0,16,0);
        for(int b=0;b<cycles*4 && !interrupted;++b) {
            Switch x; x.test=test;x.mode=mode;x.group=group;x.block=b;x.target=(b+1)%4;x.previous=previous.ready;
            x.begin=now_us();
            if(test=="pause") check(scap_pause(s.cap),"pause");
            else if(test=="reopen") s.stop();
            x.after_stop=now_us();
            check(write_y(x.target*H),"OffsetY");
            x.after_write=now_us();
            if(test=="pause") check(scap_restart(s.cap),"restart");
            else if(test=="reopen") check(s.start(8),"reopen");
            x.after_start=now_us();
            int valid=0;
            for(int i=0;valid<group && i<group+64;++i) {
                previous=capture(test,mode,group,b,i,x.target);
                if(previous.observed==x.target) {
                    if(x.first<0) x.first=previous.ready;
                    ++valid;
                } else if(x.first<0) ++x.stale;
            }
            changes.push_back(x);
            if(valid<group) throw std::runtime_error("cannot confirm target ROI from image; see baseline classification");
        }
        s.stop(); flush();
        std::printf("DONE %s %s group=%d\n",test.c_str(),mode.c_str(),group); std::fflush(stdout);
    }
    void pipeline(const std::string& mode,int group) {
        prepare(mode,true);
        std::mutex mutex; std::condition_variable cv;
        int seen=0; bool done=false; std::string error;
        int target_frames=std::max(minimum_pipeline_frames,cycles*4*group);
        std::vector<Switch> control;
        std::thread writer([&] {
            try {
                int goal=group,zone=0,b=0;
                while(true) {
                    int at=0;
                    { std::unique_lock<std::mutex> lock(mutex); cv.wait(lock,[&]{return done || seen>=goal;}); if(done) break; at=seen; }
                    zone=(zone+1)%4;
                    Switch x; x.test="live_pipeline";x.mode=mode;x.group=group;x.block=b++;x.target=zone;x.issue_frame=at;
                    x.begin=x.after_stop=now_us();
                    bool ok=write_y(zone*H);
                    x.after_write=x.after_start=now_us(); control.push_back(x);
                    if(!ok) throw std::runtime_error("pipeline OffsetY write rejected");
                    // Do not replay frame deadlines missed during a slow control transaction.
                    goal=at+group;
                }
            } catch(const std::exception& e) { std::lock_guard<std::mutex> lock(mutex);error=e.what();done=true;cv.notify_all(); }
        });
        try {
            for(int i=0;i<target_frames;++i) {
                capture("live_pipeline",mode,group,0,i,-1);
                { std::lock_guard<std::mutex> lock(mutex);if(done) break;seen=i+1; }
                cv.notify_one();
            }
        } catch(...) {
            {std::lock_guard<std::mutex> lock(mutex);done=true;} cv.notify_one();writer.join();throw;
        }
        {std::lock_guard<std::mutex> lock(mutex);done=true;} cv.notify_one(); writer.join();
        s.stop();
        for(auto& x:control) {
            for(const auto& f:batch) if(f.test=="live_pipeline" && f.ready>=x.begin) {
                if(f.observed==x.target) { x.first=f.ready;break; }
                ++x.stale;
            }
            changes.push_back(x);
        }
        flush();
        if(!error.empty()) throw std::runtime_error(error);
        std::printf("DONE live_pipeline %s group=%d\n",mode.c_str(),group);std::fflush(stdout);
    }
    bool restore() {
        if(restored || !snapshot) return true;
        s.stop(); bool ok=true;
        for(auto key:{"TLParamsLocked","OffsetX","OffsetY","Width","Height","PixelFormat","ExposureTime","AcquisitionMode","TriggerMode","AcquisitionFrameRateEnable"}) {
            try { std::string value=(std::string(key)=="TLParamsLocked" || std::string(key)=="OffsetX" || std::string(key)=="OffsetY")?"0":saved[key];
                set(key,value);status<<"RESTORED "<<key<<'='<<value<<'\n'; }
            catch(const std::exception& e) {status<<"RESTORE_ERROR "<<e.what()<<'\n';ok=false;}
        }
        for(auto key:{"OffsetX","OffsetY","TLParamsLocked"}) try {set(key,saved[key]);status<<"RESTORED "<<key<<'='<<saved[key]<<'\n';}
            catch(const std::exception& e) {status<<"RESTORE_ERROR "<<e.what()<<'\n';ok=false;}
        if(write_node=="port") {
            unsigned char bytes[4]{};size_t n=sizeof(bytes);
            bool read=scap_read_port(s.cap,scap_camera,0x3001002c,bytes,&n);
            uint32_t actual=(uint32_t(bytes[0])<<24)|(uint32_t(bytes[1])<<16)|(uint32_t(bytes[2])<<8)|bytes[3];
            if(!read || n!=4 || actual!=std::stoul(saved["OffsetY"])) {status<<"RESTORE_ERROR raw OffsetY actual="<<actual<<'\n';ok=false;}
            else status<<"RESTORED raw_OffsetY="<<actual<<'\n';
        }
        restored=true;status.flush(); return ok;
    }
    ~Rig() { try {flush();restore();} catch(...) {} }
};
int main(int argc,char** argv) {
    if(argc<2 || argc>5) {
        std::fprintf(stderr,"Usage: %s NEW_OUTPUT_DIRECTORY [CYCLES=8] [WRITE_NODE=OffsetY] [pipeline2|pipeline24|fullsplit]\n",argv[0]);
        return 2;
    }
    if(std::filesystem::exists(argv[1])) {
        std::fprintf(stderr,"Output directory already exists\n");
        return 2;
    }
    int cycles=8;
    try { if(argc>=3) cycles=std::stoi(argv[2]); }
    catch(const std::exception&) { return 2; }
    std::string write_node=argc>=4?argv[3]:"OffsetY";
    if(write_node!="OffsetY" && write_node!="OffsetYReg" && write_node!="port") return 2;
    if(cycles<1 || cycles>100) return 2;
    bool pipeline2=argc==5 && (std::string(argv[4])=="pipeline2" || std::string(argv[4])=="pipeline24");
    bool pipeline24=argc==5 && std::string(argv[4])=="pipeline24";
    bool fullsplit=argc==5 && std::string(argv[4])=="fullsplit";
    if(argc==5 && !pipeline2 && !fullsplit) return 2;
    std::signal(SIGINT,on_signal);
    std::signal(SIGTERM,on_signal);
    Rig rig(argv[1],cycles,write_node);
    int rc=0;
    try {
        rig.initialize();
        rig.reference();
        if(fullsplit) {
            rig.full_split();
        } else {
            rig.baseline();
            if(pipeline2) {
                rig.minimum_pipeline_frames=4096;
                rig.pipeline("discard",2);
                rig.pipeline("copy",2);
                if(pipeline24) {
                    rig.pipeline("discard",4);
                    rig.pipeline("copy",4);
                }
            } else {
                rig.write_only();
                for(const auto& mode:{std::string("discard"),std::string("copy")}) {
                    for(int group:{1,4,16}) {
                        rig.serial("pause",mode,group);
                        rig.serial("live_confirmed",mode,group);
                        rig.pipeline(mode,group);
                    }
                    rig.serial("reopen",mode,1);
                }
            }
        }
        rig.status<<"COMPLETE\n";
    } catch(const std::exception& e) {
        rig.status<<"ERROR "<<e.what()<<'\n';
        std::fprintf(stderr,"ERROR %s\n",e.what());
        rc=1;
    }
    rig.flush();
    if(!rig.restore()) rc=2;
    return rc;
}
