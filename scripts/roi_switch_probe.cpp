// Probe whether the camera permits ROI movement during acquisition.
#include "session.hpp"
#include "CEasyCapSEx.h"
#include <vector>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>

using Clock = std::chrono::steady_clock;
static std::atomic<bool> stopped{false};
static void signal_handler(int) { stopped = true; }
static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b-a).count();
}
int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    Session s;
    Opt opt;
    std::map<std::string,std::string> saved;
    bool snapshot = false;
    int rc = 0;
    std::array<std::vector<double>,4> refs;
    auto profile=[](const unsigned char* data,int y0) {
        std::vector<double> v;
        for(int j=0;j<32;++j) for(int i=0;i<64;++i) {
            int x=32+i*(7008-64)/64, y=y0+16+j*(1750-32)/32;
            double sum=0;
            for(int dy=0;dy<8;++dy) for(int dx=0;dx<8;++dx) sum+=data[(y+dy)*7008+x+dx];
            v.push_back(sum/64);
        }
        return v;
    };
    auto classify=[&](const unsigned char* data) {
        auto v=profile(data,0);
        std::array<double,4> scores{};
        for(int k=0;k<4;++k) {
            double sv=0,sr=0,svv=0,srr=0,svr=0;
            for(size_t i=0;i<v.size();++i) { double r=refs[k][i]; sv+=v[i]; sr+=r; svv+=v[i]*v[i]; srr+=r*r; svr+=v[i]*r; }
            double n=v.size(),den=std::sqrt(std::max(0.,(svv-sv*sv/n)*(srr-sr*sr/n)));
            scores[k]=den>0 ? (svr-sv*sr/n)/den : -2;
        }
        int best=std::max_element(scores.begin(),scores.end())-scores.begin();
        std::printf("CLASSIFY best=%d scores=%.4f,%.4f,%.4f,%.4f\n",best,scores[0],scores[1],scores[2],scores[3]);
    };
    auto set = [&](const char* key, const std::string& value) {
        if (!set_feature(s.cap, scap_camera, key, value.c_str()))
            throw std::runtime_error(std::string(key)+": "+last_err());
        std::string actual;
        if (!get_feature(s.cap, scap_camera, key, actual) || actual != value)
            throw std::runtime_error(std::string(key)+" readback="+actual+", requested="+value);
    };
    try {
        if (!s.open(opt, OpenMode::Camera)) throw std::runtime_error("open failed");
        for (auto key: {"Width", "Height", "OffsetX", "OffsetY", "PixelFormat", "ExposureTime", "TriggerMode", "AcquisitionMode", "AcquisitionFrameRateEnable"}) {
            if (!get_feature(s.cap, scap_camera, key, saved[key]))
                throw std::runtime_error(std::string("snapshot ")+key);
            std::printf("SAVED %s=%s\n", key, saved[key].c_str());
        }
        snapshot = true;
        size_t xml_size=0;
        scap_get_xml(s.cap,scap_camera,nullptr,&xml_size);
        std::printf("XML_SIZE %zu\n",xml_size);
        if (xml_size>0 && xml_size<16000000) {
            std::vector<char> xml(xml_size+1);
            size_t n=xml.size();
            if (scap_get_xml(s.cap,scap_camera,xml.data(),&n)) {
                std::ofstream out("experiments/roi-switch-20261113/camera.xml",std::ios::binary);
                out.write(xml.data(),n);
            }
        }
        for (auto key: {"Width","Height","OffsetY","SequencerFeatureSelector"}) {
            mw_node_info_t info{};
            bool ok=scap_get_node_info(s.cap,scap_camera,key,&info);
            std::printf("NODE %s ok=%d type=%d access=%d min=%lld max=%lld inc=%lld\n",key,ok,(int)info.node_type,(int)info.mode_access,(long long)info.info.integer_info.i_min,(long long)info.info.integer_info.i_max,(long long)info.info.integer_info.i_inc);
        }
        for (auto key: {"ChunkModeActive", "ChunkSelector", "ChunkEnable", "SequencerMode", "SequencerFeatureSelector", "MultipleROIEnable", "SensorWidth", "SensorHeight"}) {
            std::string value;
            bool ok = get_feature(s.cap, scap_camera, key, value);
            std::printf("FEATURE %s %s %s\n", key, ok?"available":"unavailable", ok?value.c_str():last_err());
        }
        set("AcquisitionFrameRateEnable", "0");
        set("TriggerMode", "Off");
        set("AcquisitionMode", "Continuous");
        set("OffsetX", "0"); set("OffsetY", "0");
        set("PixelFormat", "Mono8");
        set("Width", "7008"); set("Height", "1750");
        set("ExposureTime", "300");
        set("Height", "7000");
        if(!s.start(8)) throw std::runtime_error("reference start");
        for(int i=0;i<8;++i) {
            BUFFER_HANDLE h=nullptr; void* base=nullptr; size_t size=0;
            if(!scap_get_buf(s.cap,1000,&h,&base,&size)) throw std::runtime_error("reference pull");
            if(i==7) for(int k=0;k<4;++k) refs[k]=profile(static_cast<unsigned char*>(base),k*1750);
            scap_back_buffer(s.cap,h);
        }
        s.stop();
        std::ofstream preview("experiments/roi-switch-20261113/reference.pgm",std::ios::binary);
        preview<<"P5\n64 128\n255\n";
        for(const auto& v:refs) for(double x:v) { unsigned char c=std::clamp(x,0.,255.); preview.write(reinterpret_cast<char*>(&c),1); }
        preview.close();
        set("Height", "1750");
        for (int y: {0,1750,3500,5250}) {
            auto a=Clock::now(); set("OffsetY", std::to_string(y)); auto b=Clock::now();
            std::printf("STOPPED_SET y=%d write_and_read_ms=%.6f\n", y, ms(a,b));
        }
        set("OffsetY", "0");
        if (!s.start(8)) throw std::runtime_error("start failed");
        auto prev=Clock::now();
        for (int i=0; i<48 && !stopped; ++i) {
            BUFFER_HANDLE h=nullptr; void* base=nullptr; size_t size=0;
            if (!scap_get_buf(s.cap,1000,&h,&base,&size)) throw std::runtime_error("pull failed");
            auto now=Clock::now();
            cxp_buf_info_t info{};
            scap_get_buf_info(s.cap,h,&info,false);
            scap_back_buffer(s.cap,h);
            std::printf("FRAME i=%d id=%llu ts=%llu x=%zu y=%zu bytes=%zu interval_ms=%.6f\n",i,(unsigned long long)info.frame_id,(unsigned long long)info.timestamp,(size_t)info.offset_x,(size_t)info.offset_y,size,ms(prev,now));
            prev=now;
            if (i%8==7) {
                int y=(((i+1)/8)%4)*1750;
                auto a=Clock::now();
                bool ok=scap_set_integer(s.cap,scap_camera,"OffsetY",y);
                auto b=Clock::now();
                std::string error=ok?"":last_err();
                int64_t read=-1;
                bool got=scap_get_integer(s.cap,scap_camera,"OffsetY",&read);
                std::printf("LIVE_SET y=%d ok=%d write_ms=%.6f read_ok=%d read=%lld error=%s\n",y,ok,ms(a,b),got,(long long)read,error.c_str());
            }
        }
        for (int strategy=0;strategy<3;++strategy) {
            auto a=Clock::now();
            bool paused=true;
            if(strategy==0) paused=scap_set_command(s.cap,scap_camera,"AcquisitionStop");
            else if(strategy==1) paused=scap_pause(s.cap);
            else s.stop();
            auto b=Clock::now();
            int y=(strategy+1)*1750;
            bool written=scap_set_integer(s.cap,scap_camera,"OffsetY",y);
            auto c=Clock::now();
            std::string error=written?"":last_err();
            bool started=false;
            if(strategy==0) started=scap_set_command(s.cap,scap_camera,"AcquisitionStart");
            else if(strategy==1) started=scap_restart(s.cap);
            else started=s.start(8);
            auto d=Clock::now();
            std::printf("STRATEGY %d paused=%d written=%d started=%d stop_ms=%.6f write_ms=%.6f start_ms=%.6f error=%s\n",strategy,paused,written,started,ms(a,b),ms(b,c),ms(c,d),error.c_str());
            for(int j=0;j<6;++j) {
                BUFFER_HANDLE h=nullptr; void* base=nullptr; size_t size=0;
                if(!scap_get_buf(s.cap,1000,&h,&base,&size)) throw std::runtime_error("strategy pull failed");
                auto now=Clock::now();
                cxp_buf_info_t info{};
                scap_get_buf_info(s.cap,h,&info,false);
                scap_back_buffer(s.cap,h);
                std::printf("STRATEGY_FRAME method=%d j=%d y=%zu id=%llu from_stop_ms=%.6f\n",strategy,j,(size_t)info.offset_y,(unsigned long long)info.frame_id,ms(a,now));
            }
        }
        bool unlocked=scap_set_integer(s.cap,scap_camera,"TLParamsLocked",0);
        std::printf("UNLOCK ok=%d\n",unlocked);
        for(int target: {0,1750,3500,5250}) {
            auto a=Clock::now();
            bool ok=scap_set_integer(s.cap,scap_camera,"OffsetY",target);
            auto b=Clock::now();
            std::printf("UNLOCKED_SET y=%d ok=%d ms=%.6f error=%s\n",target,ok,ms(a,b),ok?"":last_err());
            for(int j=0;j<8;++j) {
                BUFFER_HANDLE h=nullptr; void* base=nullptr; size_t size=0;
                if(!scap_get_buf(s.cap,1000,&h,&base,&size)) throw std::runtime_error("unlock pull");
                cxp_buf_info_t info{}; scap_get_buf_info(s.cap,h,&info,false);
                std::printf("UNLOCKED_FRAME target=%d j=%d y=%zu id=%llu since_set_ms=%.6f\n",target,j,(size_t)info.offset_y,(unsigned long long)info.frame_id,ms(a,Clock::now()));
                classify(static_cast<unsigned char*>(base));
                scap_back_buffer(s.cap,h);
            }
        }
        scap_set_integer(s.cap,scap_camera,"TLParamsLocked",1);
    } catch (const std::exception& e) {
        std::fprintf(stderr,"ERROR %s\n",e.what()); rc=1;
    }
    s.stop();
    if (snapshot) {
        for (auto key: {"OffsetX", "OffsetY", "Width", "Height", "PixelFormat", "ExposureTime", "AcquisitionMode", "TriggerMode", "AcquisitionFrameRateEnable"}) {
            try {
                std::string value=(std::string(key)=="OffsetX" || std::string(key)=="OffsetY")?"0":saved[key];
                set(key,value);
                std::printf("RESTORED %s=%s\n",key,value.c_str());
            } catch (const std::exception& e) { std::fprintf(stderr,"RESTORE_ERROR %s\n",e.what()); rc=2; }
        }
        for (auto key: {"OffsetX","OffsetY"}) {
            try { set(key,saved[key]); std::printf("RESTORED %s=%s\n",key,saved[key].c_str()); }
            catch (const std::exception& e) { std::fprintf(stderr,"RESTORE_ERROR %s\n",e.what()); rc=2; }
        }
    }
    return rc;
}
