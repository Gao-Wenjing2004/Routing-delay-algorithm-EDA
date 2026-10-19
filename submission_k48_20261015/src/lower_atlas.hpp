#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <zlib.h>
namespace srb_v11_lower {
class LowerAtlas{
public:
    struct Header{char magic[8];uint32_t version,ns,nt,px,py,nq,codec,reserved;uint64_t values,signature,rawbytes;};
    static_assert(sizeof(Header)==64,"lower header");
    explicit LowerAtlas(const std::string&path,uint64_t signature){
        std::ifstream f(path,std::ios::binary);Header h{};
        auto read=[&](void*p,size_t n){f.read(static_cast<char*>(p),n);if(!f)throw std::runtime_error("truncated lower asset");};
        read(&h,sizeof(h));
        if(std::memcmp(h.magic,"SRB11LOW",8)||h.version!=1||h.ns<1||h.ns>496||h.nt<1||h.nt>496||h.px!=10||h.py!=12||h.nq!=4||h.codec!=1||h.reserved||h.signature!=signature
          ||h.values!=uint64_t(h.ns)*120*h.nt*4||h.rawbytes!=h.values*2||h.rawbytes>240000000)throw std::runtime_error("invalid lower asset");
        ns_=h.ns;nt_=h.nt;sm_.fill(-1);tm_.fill(-1);
        for(unsigned i=0;i<ns_;++i){uint16_t p;read(&p,2);if(p>=496||sm_[p]>=0)throw std::runtime_error("bad lower source");sm_[p]=i;}
        for(unsigned i=0;i<nt_;++i){uint16_t p;read(&p,2);if(p>=496||tm_[p]>=0)throw std::runtime_error("bad lower target");tm_[p]=i;}
        values_.resize(h.values);const size_t n=120*nt_*4,row=12*nt_*4,cell=nt_*4;
        std::vector<uint8_t>compressed,planes(n*2);
        for(unsigned s=0;s<ns_;++s){uint32_t bytes,crc;read(&bytes,4);read(&crc,4);
            if(!bytes||bytes>compressBound(n*2))throw std::runtime_error("bad lower frame length");
            compressed.resize(bytes);read(compressed.data(),bytes);uLongf actual=n*2;
            if(uncompress(planes.data(),&actual,compressed.data(),bytes)!=Z_OK||actual!=n*2)throw std::runtime_error("bad lower frame");
            uint16_t*out=values_.data()+s*n;
            for(size_t j=0;j<n;++j)out[j]=planes[j]|uint16_t(planes[n+j])<<8;
            for(unsigned x=1;x<10;++x)for(size_t j=0;j<row;++j)out[x*row+j]+=out[(x-1)*row+j];
            for(unsigned x=0;x<10;++x)for(unsigned y=1;y<12;++y)for(size_t j=0;j<cell;++j)out[x*row+y*cell+j]+=out[x*row+(y-1)*cell+j];
            if(crc32(0,reinterpret_cast<const Bytef*>(out),n*2)!=crc)throw std::runtime_error("lower CRC mismatch");
        }
        if(f.peek()!=std::char_traits<char>::eof())throw std::runtime_error("trailing lower bytes");
    }
    const uint16_t*get(unsigned source,unsigned target,unsigned phase)const{
        const int s=sm_[source],t=tm_[target];if(s<0||t<0)return nullptr;
        return values_.data()+((uint64_t(s)*120+phase)*nt_+t)*4;
    }
    size_t memory_bytes()const{return values_.size()*2+sizeof(sm_)+sizeof(tm_);}
private:
    unsigned ns_,nt_;std::array<int16_t,496>sm_,tm_;std::vector<uint16_t>values_;
};
}
