#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <filesystem>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <stdexcept>
#include <string>
#include <vector>
#include <zlib.h>
#include "table_format.hpp"
#include "tail_parameters.hpp"

// Endpoint must expose x,y,port. Caller retains its own physical/block guard.
// Loaded distances are ideal finite-window Atlas distances, never claimed exact
// for Gap/Block/finite-chip routing. Runtime tail lookup has no graph search.
namespace srb_structural {
class VerticalTail {
public:
    explicit VerticalTail(const std::string& path) {
        source_.fill(-1);target_.fill(-1);source_offsets_.fill(UINT64_MAX);
        std::filesystem::path input_path=path;
#ifdef SRB_EMBEDDED_ATLAS
#ifdef _WIN32
        wchar_t module[32768];const DWORD chars=GetModuleFileNameW(nullptr,module,32768);
        if(!chars||chars>=32768)throw std::runtime_error("cannot locate embedded executable");
        input_path=std::filesystem::path(module);
#else
        input_path="/proc/self/exe";
#endif
#endif
        std::ifstream in(input_path,std::ios::binary);
        if(!in)throw std::runtime_error("cannot open structural Atlas");
        in.seekg(0,std::ios::end);const uint64_t file_bytes=uint64_t(in.tellg());
        uint64_t offset=0,expected_end=file_bytes;
#ifdef SRB_EMBEDDED_ATLAS
        struct Footer{char magic[16];uint64_t offset,bytes;}footer{};
        static_assert(sizeof(Footer)==32,"footer layout");
        if(file_bytes<32)throw std::runtime_error("missing embedded footer");
        in.seekg(file_bytes-32);in.read(reinterpret_cast<char*>(&footer),32);
        if(!in||std::memcmp(footer.magic,"SRB9TAB_20261012",16)||footer.offset%4096
          ||footer.offset>file_bytes-32||footer.bytes!=file_bytes-32-footer.offset)
            throw std::runtime_error("invalid embedded footer");
        offset=footer.offset;expected_end=file_bytes-32;
#endif
        if(expected_end-offset<96)throw std::runtime_error("truncated structural Atlas");
        in.seekg(offset);
        srb_v8::CompressedHeader ch{};in.read(reinterpret_cast<char*>(&ch),32);
        if(!in||std::memcmp(ch.magic,"SRB8ZLB\0",8)||ch.version!=2||ch.reserved!=72||ch.raw_bytes>500000000)
            throw std::runtime_error("bad structural vertical asset");
        if(ch.compressed_bytes!=expected_end-offset-32)throw std::runtime_error("compressed size mismatch");
        in.read(reinterpret_cast<char*>(&header_),64);
        const auto& h=header_;
        if(!in||std::memcmp(h.magic,"SRB8TAB\0",8)||h.version!=1||h.radius!=72||h.ports!=496||h.bits!=16||h.record_bytes!=32
          ||h.sources!=80||h.targets!=256||(!h.cells||h.cells>145*145)||h.values!=uint64_t(h.sources)*h.cells*h.targets||h.payload_bytes!=2*h.values)
            throw std::runtime_error("bad structural vertical dimensions");
        if(ch.raw_bytes!=64+2ull*(h.sources+h.targets+145*145)+h.payload_bytes)
            throw std::runtime_error("decoded size mismatch");
        for(unsigned i=0;i<h.sources;++i){uint16_t id;in.read(reinterpret_cast<char*>(&id),2);if(id>=496||source_[id]>=0)throw std::runtime_error("bad source");source_[id]=i;source_offsets_[id]=uint64_t(i)*h.cells*h.targets;}
        for(unsigned i=0;i<h.targets;++i){uint16_t id;in.read(reinterpret_cast<char*>(&id),2);if(id>=496||target_[id]>=0)throw std::runtime_error("bad target");target_[id]=i;}
        in.read(reinterpret_cast<char*>(grid_.data()),2*grid_.size());
        values_.resize(h.values);
        const size_t count=145*145*h.targets,row=145*h.targets;
        std::vector<uint8_t> shuffled(2*count),encoded;
        std::vector<uint16_t> matrix(count);
        for(unsigned s=0;s<h.sources;++s){
            uint32_t bytes;in.read(reinterpret_cast<char*>(&bytes),4);if(!in||bytes>12000000)throw std::runtime_error("bad frame length");
            encoded.resize(bytes);in.read(reinterpret_cast<char*>(encoded.data()),bytes);
            uLongf actual=shuffled.size();
            if(!in||uncompress(shuffled.data(),&actual,encoded.data(),bytes)!=Z_OK||actual!=shuffled.size())throw std::runtime_error("bad zlib frame");
            for(size_t j=0;j<count;++j)matrix[j]=shuffled[j]|uint16_t(shuffled[count+j])<<8;
            for(unsigned y=1;y<145;++y)for(size_t j=0;j<row;++j)matrix[y*row+j]+=matrix[(y-1)*row+j];
            for(unsigned y=0;y<145;++y)for(unsigned x=1;x<145;++x)for(unsigned target=0;target<h.targets;++target)
                matrix[(y*145+x)*h.targets+target]+=matrix[(y*145+x-1)*h.targets+target];
            for(unsigned cell=0;cell<grid_.size();++cell)if(grid_[cell]!=65535){
                if(grid_[cell]>=h.cells)throw std::runtime_error("bad grid cell");
                std::memcpy(values_.data()+(uint64_t(s)*h.cells+grid_[cell])*h.targets,matrix.data()+uint64_t(cell)*h.targets,h.targets*2);
            }
        }
        if(uint64_t(in.tellg())!=expected_end)throw std::runtime_error("trailing asset bytes");
        // Precompute actual-displacement metadata: runtime avoids division and
        // angle calculation. Each row is a selected representative + additive cost.
        geometry_.resize(239*1099);
        for(int dx=-119;dx<=119;++dx)for(int dy=-549;dy<=549;++dy){
            Geometry& g=geometry_[(dx+119)*1099+dy+549];
            const int ax=std::abs(dx),ay=std::abs(dy);
            if(ax>72||ay<=72){g.cell=65535;continue;}
            const int repeat=(ay-60)/12;const int reduced=dy-(dy>0?1:-1)*repeat*12;
            g.cell=grid_[(reduced+72)*145+dx+72];
            g.extra=repeat*(dy>0?127:125);
            const unsigned quadrant=(dx>0)+2*(dx<0)+3*(dy>0)+6*(dy<0);
            g.shape=quadrant*8+std::min(7,8*ax/(ax+ay+1));
        }
    }
    uint16_t lookup(unsigned source,unsigned target,int dx,int reduced_dy)const {
        if(source>=496||target>=496||source_[source]<0||target_[target]<0||std::abs(dx)>72||std::abs(reduced_dy)>72)return 65535;
        const unsigned cell=grid_[(reduced_dy+72)*145+dx+72];if(cell==65535)return 65535;
        return values_[source_offsets_[source]+(uint64_t(cell)<<8)+target_[target]];
    }
    template<class Endpoint> bool predict(const Endpoint&a,const Endpoint&b,int gap,uint32_t&out)const {
        const int dx=int(b.x)-a.x,dy=int(b.y)-a.y;
        const Geometry& g=geometry_[(dx+119)*1099+dy+549];
        if(g.cell==65535||source_[a.port]<0||target_[b.port]<0)return false;
        const unsigned v=values_[source_offsets_[a.port]+(uint64_t(g.cell)<<8)+target_[b.port]];
        if(v==65535)return false;
        const int c=int(source_correction[a.port*72+g.shape])+target_correction[b.port*72+g.shape];
        // Explicit floor division matches Python even for negative corrections.
        const int rounded=c>=-4?(c+4)/8:-((-c+3)/8);
        out=std::max(0,int(v)+int(g.extra)+gap+rounded);return true;
    }
    size_t memory_bytes()const{return values_.size()*2+geometry_.size()*sizeof(Geometry)+sizeof(source_)+sizeof(target_)+sizeof(grid_)+sizeof(source_offsets_);}
private:
    struct Geometry { uint16_t cell=65535,extra=0;uint8_t shape=0;uint8_t pad=0; };
    srb_v8::Header header_{};
    std::array<int16_t,496> source_,target_;
    std::array<uint64_t,496> source_offsets_;
    std::array<uint16_t,145*145>grid_;
    std::vector<uint16_t>values_;
    std::vector<Geometry>geometry_;
};
}
