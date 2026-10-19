#pragma once
#include "lower_atlas.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <lzma.h>
#include <zlib.h>

namespace srb_v11_short_k32 {
struct Header {char magic[8];uint32_t version,codec,ports,sources,targets,prototypes,radius,search,cells,geometry;uint64_t values,signature;};
static_assert(sizeof(Header)==64,"short header layout");
struct Frame {uint32_t xz_bytes,bitmap_bytes,crc,finite;};

// Ideal finite-Atlas costs at frozen target medoids. The dual is ONLY a
// reversible storage predictor. Projection/bias and dualget are approximate
// teachers, with no physical Block-path or Lower-bound certificate.
class Table {
public:
    Table(const std::string&path,const std::string&dual_path,uint64_t signature=0){load(path,dual_path,signature);}
    void load(const std::string&path,const std::string&dual_path,uint64_t signature=0){
        std::ifstream in(path,std::ios::binary);if(!in)fail("short open failed");
        auto read=[&](void*p,size_t n){in.read(static_cast<char*>(p),std::streamsize(n));if(!in)fail("short truncated");};
        read(&header_,64);const auto&h=header_;
        if(std::memcmp(h.magic,"SRB11SK2",8)||h.version!=1||h.codec!=1||h.ports!=496||h.sources<1||h.sources>496||h.targets<1||h.targets>496
           ||h.prototypes!=32||h.radius!=72||h.search!=80||h.cells!=21025||h.geometry!=72||h.values!=uint64_t(h.sources)*h.cells*32
           ||h.values*2>1700000000ull||(signature&&h.signature!=signature))fail("short header invalid");
        sources_.resize(h.sources);targets_.resize(h.targets);medoids_.resize(32);assignment_.resize(h.targets);bias_.resize(4*120*h.targets);
        read(sources_.data(),sources_.size()*2);read(targets_.data(),targets_.size()*2);read(medoids_.data(),64);read(assignment_.data(),assignment_.size());read(bias_.data(),bias_.size()*2);
        sm_.fill(-1);tm_.fill(-1);
        for(unsigned i=0;i<h.sources;++i){unsigned p=sources_[i];if(p>=496||sm_[p]>=0)fail("short source IDs");sm_[p]=i;}
        for(unsigned i=0;i<h.targets;++i){unsigned p=targets_[i];if(p>=496||tm_[p]>=0)fail("short target IDs");tm_[p]=i;}
        std::array<bool,496>seen{};for(unsigned p:medoids_){if(p>=496||tm_[p]<0||seen[p])fail("short medoids invalid");seen[p]=true;}
        for(unsigned p:assignment_)if(p>=32)fail("short assignment invalid");
        geometry_.resize((h.cells+7)/8);read(geometry_.data(),geometry_.size());
        if(geometry_.back()&0xfe)fail("short geometry padding");
        for(unsigned i=0;i<h.cells;++i){int x=int(i%145)-72,y=int(i/145)-72;bool expected=std::abs(x)+std::abs(y)<=72&&(std::abs(x)>24||std::abs(y)>24);
            if(expected!=bool((geometry_[i>>3]>>(i&7))&1))fail("short geometry mismatch");}
        grid_.assign(h.cells,65535);stored_cells_=0;
        for(unsigned i=0;i<h.cells;++i)if((geometry_[i>>3]>>(i&7))&1)grid_[i]=uint16_t(stored_cells_++);
        if(stored_cells_!=8112)fail("short coverage cell count");
        dual_=std::make_unique<srb_v11_lower::LowerAtlas>(dual_path,h.signature);
        const size_t n=size_t(h.cells)*32,bitbytes=(n+7)/8;
        values_.resize(size_t(h.sources)*stored_cells_*32);std::vector<uint16_t>decoded(n);
        std::vector<uint8_t>planes(n*2),finite_bits(bitbytes),compressed;
        for(unsigned sr=0;sr<h.sources;++sr){
            Frame f{};read(&f,sizeof(f));
            if(!f.xz_bytes||f.xz_bytes>lzma_stream_buffer_bound(n*2)||!f.bitmap_bytes||f.bitmap_bytes>compressBound(bitbytes)||f.finite>n)fail("short frame invalid");
            compressed.resize(f.xz_bytes);read(compressed.data(),compressed.size());uint64_t limit=64ull<<20;size_t ip=0,op=0;
            if(lzma_stream_buffer_decode(&limit,0,nullptr,compressed.data(),&ip,compressed.size(),planes.data(),&op,planes.size())!=LZMA_OK||ip!=compressed.size()||op!=planes.size())fail("short XZ decode");
            compressed.resize(f.bitmap_bytes);read(compressed.data(),compressed.size());uLongf got=finite_bits.size();
            if(uncompress(finite_bits.data(),&got,compressed.data(),compressed.size())!=Z_OK||got!=finite_bits.size())fail("short bitmap decode");
            uint16_t*out=decoded.data();
            for(size_t i=0;i<n;++i){uint16_t z=planes[i]|uint16_t(planes[n+i])<<8;out[i]=uint16_t((z>>1)^uint16_t(-int(z&1)));}
            for(unsigned y=0;y<145;++y)for(unsigned x=1;x<145;++x)for(unsigned t=0;t<32;++t){size_t i=(size_t(y)*145+x)*32+t;out[i]=uint16_t(out[i]+out[i-32]);}
            for(unsigned y=1;y<145;++y)for(unsigned x=0;x<145;++x)for(unsigned t=0;t<32;++t){size_t i=(size_t(y)*145+x)*32+t;out[i]=uint16_t(out[i]+out[i-145*32]);}
            unsigned finite=0;
            for(unsigned cell=0;cell<h.cells;++cell){int x=int(cell%145)-72,y=int(cell/145)-72;bool covered=(geometry_[cell>>3]>>(cell&7))&1;
                for(unsigned t=0;t<32;++t){size_t i=size_t(cell)*32+t;bool valid=(finite_bits[i>>3]>>(i&7))&1;
                    if(!valid){if(out[i]!=0)fail("short missing payload must be zero");out[i]=65535;continue;}
                    if(!covered)fail("short finite value outside geometry");++finite;
                    int predicted=0;if(!predictor(sources_[sr],t,x,y,predicted))fail("short predictor unavailable");
                    int r=out[i];if(r>=32768)r-=65536;int value=predicted+r;
                    if(value<0||value>=65535)fail("short reconstructed cost outside uint16");out[i]=uint16_t(value);
                }
            }
            if(finite!=f.finite||crc32(0,reinterpret_cast<const Bytef*>(out),uInt(n*2))!=f.crc)fail("short cost CRC mismatch");
            for(unsigned cell=0;cell<h.cells;++cell)if(grid_[cell]!=65535)
                std::memcpy(values_.data()+(uint64_t(sr)*stored_cells_+grid_[cell])*32,out+size_t(cell)*32,64);
        }
        if(in.peek()!=EOF)fail("short trailing bytes");
    }
    struct PreparedQuery{const uint16_t*value;int bias;};
    bool prepare(unsigned source,unsigned target,int dx,int dy,PreparedQuery&q)const{
        if(source>=496||target>=496||dx<-72||dx>72||dy<-72||dy>72)return false;
        const int s=sm_[source],t=tm_[target];if(s<0||t<0)return false;
        const unsigned cell=unsigned(dy+72)*145+unsigned(dx+72);
        if(grid_[cell]==65535)return false;
        q.value=values_.data()+(uint64_t(s)*stored_cells_+grid_[cell])*32+assignment_[t];
        q.bias=target_bias(t,dx,dy);return true;
    }
    void prefetch(const PreparedQuery&q)const{
#if defined(__GNUC__) || defined(__clang__)
        __builtin_prefetch(q.value,0,3);
#endif
    }
    bool query_prepared(const PreparedQuery&q,uint32_t&out)const{
        const uint16_t v=*q.value;if(v==65535)return false;
        out=uint32_t(std::max(0,int(v)+q.bias));return true;
    }
    // Input source and original target IDs; result is assigned MEDOID Arc cost
    // only, without target bias, physical forced-Net extra, or Gap reference.
    bool query_medoid(unsigned source,unsigned target,int dx,int dy,uint32_t&out)const{
        if(source>=496||target>=496||dx<-72||dx>72||dy<-72||dy>72)return false;
        int s=sm_[source],t=tm_[target];if(s<0||t<0)return false;
        unsigned cell=unsigned(dy+72)*145+unsigned(dx+72);
        if(grid_[cell]==65535)return false;
        uint16_t v=values_[(uint64_t(s)*stored_cells_+grid_[cell])*32+assignment_[t]];if(v==65535)return false;out=v;return true;
    }
    // Approximate ideal original-target Arc cost: exact retained medoid cost
    // plus independently architecture-derived target/quadrant/phase bias.
    bool query(unsigned source,unsigned target,int dx,int dy,uint32_t&out)const{
        uint32_t v=0;if(!query_medoid(source,target,dx,dy,v))return false;
        out=uint32_t(std::max(0,int(v)+target_bias(tm_[target],dx,dy)));return true;
    }
    // All geometry support of the phase dual, restricted to these110 sources
    // and256 targets. Adding medoid projection/bias makes this approximate,
    // and callers must not impose a nonnegative residual/LB clamp because of it.
    bool dualget(unsigned source,unsigned target,int dx,int dy,uint32_t&out)const{
        if(source>=496||target>=496||dx<-119||dx>119||dy<-549||dy>549||sm_[source]<0||tm_[target]<0)return false;
        int t=tm_[target],v=0;if(!predictor(source,assignment_[t],dx,dy,v))return false;
        out=uint32_t(std::max(0,v+target_bias(t,dx,dy)));return true;
    }
    const Header&header()const{return header_;}
    const std::vector<uint16_t>&values()const{return values_;}
    const std::vector<uint16_t>&sources()const{return sources_;}
    const std::vector<uint16_t>&targets()const{return targets_;}
    const std::vector<uint16_t>&medoids()const{return medoids_;}
    const std::vector<uint8_t>&assignment()const{return assignment_;}
    const std::vector<int16_t>&architecture_bias()const{return bias_;} // Read-only audit API; lookup unchanged.
    // Decoder/audit helper; serialized canvas cell index, not a request lookup.
    uint16_t medoid_value(unsigned source_row,unsigned cell,unsigned proto)const{
        if(source_row>=header_.sources||cell>=header_.cells||proto>=32||grid_[cell]==65535)return 65535;
        return values_[(uint64_t(source_row)*stored_cells_+grid_[cell])*32+proto];
    }
    unsigned stored_cells()const{return stored_cells_;}
    size_t memory_bytes()const{return values_.size()*2+bias_.size()*2+geometry_.size()+grid_.size()*2+assignment_.size()+(sources_.size()+targets_.size()+medoids_.size())*2+sizeof(sm_)+sizeof(tm_)+dual_->memory_bytes();}
private:
    static unsigned phase(int dx,int dy){return unsigned((dx%10+10)%10)*12+unsigned((dy%12+12)%12);}
    int target_bias(unsigned t,int dx,int dy)const{unsigned q=(dx<0)+2*(dy<0);return bias_[(q*120+phase(dx,dy))*header_.targets+t];}
    bool predictor(unsigned source,unsigned proto,int dx,int dy,int&out)const{
        unsigned p=phase(dx,dy);const uint16_t*r=dual_->get(source,medoids_[proto],p);if(!r)return false;
        const int cx=(dx-int(p/12))/10,cy=(dy-int(p%12))/12;
        const int east=cx*125,north=cy*127,south=cy*-125;int best=-1000000;
        for(unsigned q=0;q<4;++q)if(r[q]!=32767){int it=r[q];if(it>=32768)it-=65536;best=std::max(best,(q&1?-east:east)+(q&2?south:north)+it);}
        if(best==-1000000)return false;out=std::max(0,best);return true;
    }
    [[noreturn]]static void fail(const char*s){throw std::runtime_error(s);}
    Header header_{};unsigned stored_cells_=0;std::array<int16_t,496>sm_{},tm_{};
    std::vector<uint16_t>sources_,targets_,medoids_,values_,grid_;std::vector<uint8_t>assignment_,geometry_;std::vector<int16_t>bias_;
    std::unique_ptr<srb_v11_lower::LowerAtlas>dual_;
};
}
