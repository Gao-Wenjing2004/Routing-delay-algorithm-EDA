#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <zlib.h>

namespace srb_v11_combined {
struct PrototypeHeader {
    char magic[8];
    int32_t version,codec,sources,targets,prototypes,xmin,xmax,phase_y,min_qy,reserved;
    uint64_t values,graph_signature;
};
static_assert(sizeof(PrototypeHeader)==64,"prototype header layout");
struct PrototypeFrame { uint32_t bytes,crc; };

// Architecture-only target medoids and mean phase offsets approximate the
// target terminal. The selected Y-reference V carrier may be suboptimal at
// other dy. Actual transverse dx is preserved, including small/zero values.
// Caller owns physical normalization, safe/long/local/identity gates, extra,
// gap and the separately fit combined residual.
class PrototypeTable {
public:
    explicit PrototypeTable(const std::string&path,uint64_t signature=0) { load(path,signature); }
    void load(const std::string&path,uint64_t signature=0) {
        std::ifstream in(path,std::ios::binary);if(!in)fail("combined prototype open failed");
        auto read=[&](void*dest,size_t n){in.read(static_cast<char*>(dest),std::streamsize(n));if(!in)fail("combined prototype truncated");};
        read(&header_,sizeof(header_));const auto&h=header_;
        if(std::memcmp(h.magic,"SRB11PRT",8)||h.version!=1||h.codec!=1||h.sources<=0||h.sources>496||h.targets<=0||h.targets>496||h.prototypes!=32||h.xmin!=-94||h.xmax!=94||h.phase_y!=12||h.min_qy!=6||h.reserved
           ||(signature&&h.graph_signature!=signature))fail("combined prototype header invalid");
        const size_t frame_values=size_t(189)*12*h.prototypes;
        if(h.values!=frame_values*2*h.sources||h.values>600000000)fail("combined prototype dimensions invalid");
        std::vector<uint16_t>sources(h.sources),targets(h.targets),prototypes(h.prototypes);
        read(sources.data(),sources.size()*2);read(targets.data(),targets.size()*2);read(slopes_,sizeof(slopes_));read(prototypes.data(),prototypes.size()*2);
        source_row_.fill(-1);target_row_.fill(-1);
        for(int i=0;i<h.sources;++i){unsigned p=sources[i];if(p>=496||source_row_[p]>=0)fail("combined prototype source IDs invalid");source_row_[p]=int16_t(i);}
        for(int i=0;i<h.targets;++i){unsigned p=targets[i];if(p>=496||target_row_[p]>=0)fail("combined prototype target IDs invalid");target_row_[p]=int16_t(i);}
        std::array<bool,496>prototype_seen{};
        for(unsigned p:prototypes)if(p>=496||target_row_[p]<0||prototype_seen[p])fail("combined prototype medoid IDs invalid");else prototype_seen[p]=true;
        assignment_.resize(h.targets);bias_.resize(size_t(2)*12*h.targets);
        read(assignment_.data(),assignment_.size());read(bias_.data(),bias_.size()*2);
        for(unsigned p:assignment_)if(p>=unsigned(h.prototypes))fail("combined prototype assignment invalid");
        for(const auto&row:slopes_)for(uint16_t value:row)if(value<100||value>200)fail("combined prototype V loop invalid");
        values_.resize(size_t(h.values));std::vector<uint8_t>compressed,planes(frame_values*2);
        for(int frame=0;frame<2*h.sources;++frame){
            PrototypeFrame f{};read(&f,sizeof(f));
            if(!f.bytes||f.bytes>compressBound(uLong(planes.size())))fail("combined prototype frame size invalid");
            compressed.resize(f.bytes);read(compressed.data(),compressed.size());uLongf n=uLongf(planes.size());
            if(uncompress(planes.data(),&n,compressed.data(),uLong(compressed.size()))!=Z_OK||n!=planes.size())fail("combined prototype zlib failed");
            uint16_t*out=values_.data()+size_t(frame)*frame_values;
            for(size_t i=0;i<frame_values;++i)out[i]=uint16_t(planes[i])|(uint16_t(planes[frame_values+i])<<8);
            const size_t xstride=size_t(12)*h.prototypes;
            for(int x=10;x<189;++x)for(size_t j=0;j<xstride;++j){size_t i=size_t(x)*xstride+j;out[i]=uint16_t(out[i]+out[i-10*xstride]);}
            for(int x=0;x<189;++x)for(int y=1;y<12;++y)for(int t=0;t<h.prototypes;++t){size_t i=(size_t(x)*12+y)*h.prototypes+t;out[i]=uint16_t(out[i]+out[i-h.prototypes]);}
            if(crc32(0,reinterpret_cast<const Bytef*>(out),uInt(frame_values*2))!=f.crc)fail("combined prototype CRC mismatch");
        }
        if(in.peek()!=EOF)fail("combined prototype trailing bytes");
    }
    bool query(unsigned source,unsigned target,int dx,int dy,uint32_t&out)const {
        if(source>=496||target>=496||dx<-94||dx>94)return false;
        const int sr=source_row_[source],tr=target_row_[target],ay=dy<0?-dy:dy;
        if(sr<0||tr<0||ay<72)return false;
        const unsigned direction=dy<0,q=unsigned(ay)/12,ry=unsigned(ay)%12;
        const size_t index=(((size_t(direction)*header_.sources+unsigned(sr))*189+unsigned(dx+94))*12+ry)*32+assignment_[tr];
        const uint16_t packed=values_[index];if(packed==65535)return false;
        const unsigned lane=packed>>12;if(lane>=8)fail("combined prototype packed mode invalid");
        int intercept=packed&4095;if(intercept>=2048)intercept-=4096;
        const int value=intercept+int(slopes_[direction][lane])*q+bias_[(direction*12+ry)*header_.targets+unsigned(tr)];
        if(value<0)return false;out=uint32_t(value);return true;
    }
    size_t memory_bytes()const { return values_.size()*2+bias_.size()*2+assignment_.size()+sizeof(source_row_)+sizeof(target_row_)+sizeof(slopes_); }
    const PrototypeHeader&header()const{return header_;}
private:
    [[noreturn]]static void fail(const char*message){throw std::runtime_error(message);}
    PrototypeHeader header_{};
    std::array<int16_t,496>source_row_{},target_row_{};
    uint16_t slopes_[2][8]{};
    std::vector<uint16_t>values_;
    std::vector<uint8_t>assignment_;
    std::vector<int16_t>bias_;
};
}
