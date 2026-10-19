#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <zlib.h>
#include <lzma.h>

namespace srb_v11_local_expand {
// Codec4 uses SRB11LC4/version2 plus an8B xstep/ystep/metadataCRC extension.
// Legacy SRB11LC2 codecs1/3 remain strict and byte-equivalent in values/query API.
class Local {
public:
    struct Header {
        char magic[8];
        uint32_t version,codec,ports,sources,targets,radius,search_radius,cells,metadata_bytes,flags;
        uint64_t values,signature;
    };
    static_assert(sizeof(Header)==64,"expanded Local header layout");
    explicit Local(const std::string&path,uint64_t signature=0,uint64_t offset=0,uint64_t bytes=0){load(path,signature,offset,bytes);}
    void load(const std::string&path,uint64_t signature=0,uint64_t offset=0,uint64_t bytes=0){
        std::ifstream f(path,std::ios::binary);if(!f)fail("cannot open expanded Local");
        f.seekg(0,std::ios::end);const auto end=f.tellg();if(end<0)fail("expanded Local size unavailable");
        const uint64_t total_bytes=uint64_t(end);
        if(offset>total_bytes||(bytes&&bytes>total_bytes-offset))fail("expanded Local range outside file");
        const uint64_t available=bytes?bytes:total_bytes-offset;uint64_t consumed=0;
        auto read=[&](void*p,size_t n){if(n>available-consumed)fail("truncated expanded Local");f.read(static_cast<char*>(p),n);if(!f)fail("expanded Local read failed");consumed+=n;};
        f.seekg(offset,std::ios::beg);read(&header_,64);const auto&h=header_;
        const bool codec4=std::memcmp(h.magic,"SRB11LC4",8)==0&&h.version==2&&h.codec==4;
        const bool legacy=std::memcmp(h.magic,"SRB11LC2",8)==0&&h.version==1&&(h.codec==1||h.codec==3);
        if((!codec4&&!legacy)||h.ports!=496||h.sources<1||h.sources>496||h.targets<1||h.targets>496
          ||h.radius!=72||h.search_radius!=80||h.cells<1||h.cells>145*145||h.flags
          ||h.metadata_bytes!=2*(h.sources+h.targets+145*145)||h.values!=uint64_t(h.sources)*h.cells*h.targets
          ||(signature&&h.signature!=signature)||(codec4&&(h.sources!=110||h.targets!=256||h.cells!=2401)))
            fail("expanded Local header mismatch");
        struct Codec4Extension {uint16_t xstep,ystep;uint32_t metadata_crc;};
        static_assert(sizeof(Codec4Extension)==8,"codec4 extension layout");
        Codec4Extension ext{};
        if(codec4){read(&ext,8);if(ext.xstep!=10||ext.ystep!=1)fail("expanded Local codec4 predictor invalid");}
        const unsigned xstep=codec4?ext.xstep:1,ystep=codec4?ext.ystep:1;
        const uint64_t frame_values=uint64_t(h.cells)*h.targets;
        if(h.values*2+frame_values*4+compressBound(uLong(frame_values*2))>1850000000ull)fail("expanded Local memory budget exceeds limit");
        sources_.resize(h.sources);targets_.resize(h.targets);grid_.resize(145*145);
        read(sources_.data(),h.sources*2);read(targets_.data(),h.targets*2);read(grid_.data(),grid_.size()*2);
        if(codec4){
            uLong metadata_crc=crc32(0,nullptr,0);
            metadata_crc=crc32(metadata_crc,reinterpret_cast<const Bytef*>(sources_.data()),uInt(sources_.size()*2));
            metadata_crc=crc32(metadata_crc,reinterpret_cast<const Bytef*>(targets_.data()),uInt(targets_.size()*2));
            metadata_crc=crc32(metadata_crc,reinterpret_cast<const Bytef*>(grid_.data()),uInt(grid_.size()*2));
            if(metadata_crc!=ext.metadata_crc)fail("expanded Local codec4 metadata CRC differs");
        }

        source_row_.fill(-1);target_row_.fill(-1);
        for(unsigned i=0;i<h.sources;++i){const auto p=sources_[i];if(p>=496||source_row_[p]>=0)fail("expanded Local duplicate source");source_row_[p]=int16_t(i);}
        for(unsigned i=0;i<h.targets;++i){const auto p=targets_[i];if(p>=496||target_row_[p]>=0)fail("expanded Local duplicate target");target_row_[p]=int16_t(i);}
        std::vector<uint16_t>left(h.cells,65535),up(h.cells,65535),diag(h.cells,65535);
        unsigned found=0;xmin_=ymin_=72;xmax_=ymax_=-72;
        for(int y=0;y<145;++y)for(int x=0;x<145;++x){
            const uint16_t id=grid_[y*145+x];
            if(codec4&&((id!=65535)!=(x>=48&&x<=96&&y>=48&&y<=96)))fail("expanded Local codec4 requires frozen Square24");
            if(id==65535)continue;
            if(id!=found++||id>=h.cells)fail("expanded Local cell IDs must be complete row order");
            xmin_=std::min(xmin_,x-72);xmax_=std::max(xmax_,x-72);ymin_=std::min(ymin_,y-72);ymax_=std::max(ymax_,y-72);
            if(unsigned(x)>=xstep)left[id]=grid_[y*145+x-xstep];
            if(unsigned(y)>=ystep)up[id]=grid_[(y-ystep)*145+x];
            if(unsigned(x)>=xstep&&unsigned(y)>=ystep)diag[id]=grid_[(y-ystep)*145+x-xstep];
            if((left[id]!=65535&&left[id]>=id)||(up[id]!=65535&&up[id]>=id)||(diag[id]!=65535&&diag[id]>=id))fail("expanded Local predictor is not causal");
        }
        if(found!=h.cells)fail("expanded Local cell count mismatch");
        dense_=h.cells==unsigned((xmax_-xmin_+1)*(ymax_-ymin_+1));width_=unsigned(xmax_-xmin_+1);
        values_.resize(h.values);std::vector<uint8_t>compressed,planes(frame_values*2);
        const size_t n=size_t(frame_values);
        for(unsigned s=0;s<h.sources;++s){
            uint32_t blob_bytes,crc;read(&blob_bytes,4);read(&crc,4);
            if(!blob_bytes||blob_bytes>(h.codec==1?compressBound(uLong(n*2)):lzma_stream_buffer_bound(n*2)))fail("expanded Local invalid compressed length");
            compressed.resize(blob_bytes);read(compressed.data(),blob_bytes);uLongf got=uLongf(planes.size());
            if(h.codec==1){
                if(uncompress(planes.data(),&got,compressed.data(),blob_bytes)!=Z_OK||got!=planes.size())fail("expanded Local decompression failed");
            }else{
                uint64_t memlimit=64ull<<20;size_t inpos=0,outpos=0;
                const auto status=lzma_stream_buffer_decode(&memlimit,0,nullptr,compressed.data(),&inpos,compressed.size(),planes.data(),&outpos,planes.size());
                if(status!=LZMA_OK||inpos!=compressed.size()||outpos!=planes.size())fail("expanded Local XZ decompression failed");
            }
            uint16_t*out=values_.data()+uint64_t(s)*n;
            for(size_t j=0;j<n;++j){
                const uint16_t bits=uint16_t(planes[j])|(uint16_t(planes[n+j])<<8);
                out[j]=h.codec==1?bits:uint16_t(int(bits>>1)^-int(bits&1));
            }
            for(unsigned c=0;c<h.cells;++c){
                auto*p=out+uint64_t(c)*h.targets;
                const uint16_t*l=left[c]==65535?nullptr:out+uint64_t(left[c])*h.targets;
                const uint16_t*u=up[c]==65535?nullptr:out+uint64_t(up[c])*h.targets;
                const uint16_t*d=diag[c]==65535?nullptr:out+uint64_t(diag[c])*h.targets;
                for(unsigned t=0;t<h.targets;++t)p[t]=uint16_t(unsigned(p[t])+(l?l[t]:0)+(u?u[t]:0)-(d?d[t]:0));
            }
            if(crc32(0,reinterpret_cast<const Bytef*>(out),uInt(n*2))!=crc)fail("expanded Local raw CRC differs");
        }
        if(consumed!=available)fail("expanded Local trailing bytes");
    }
    uint16_t get(unsigned source,unsigned target,int dx,int dy)const{
        if(source>=496||target>=496)return 65535;
        const int s=source_row_[source],t=target_row_[target];if(s<0||t<0)return 65535;
        unsigned cell;
        if(dense_){const unsigned x=unsigned(dx-xmin_),y=unsigned(dy-ymin_);if(x>=width_||y>=unsigned(ymax_-ymin_+1))return 65535;cell=y*width_+x;}
        else{const unsigned x=unsigned(dx+72),y=unsigned(dy+72);if(x>=145||y>=145)return 65535;cell=grid_[y*145+x];if(cell==65535)return 65535;}
        return values_[(uint64_t(s)*header_.cells+cell)*header_.targets+unsigned(t)];
    }
    const uint16_t*get_pointer(unsigned source,unsigned target,int dx,int dy)const{
        if(source>=496||target>=496)return nullptr;
        const int s=source_row_[source],t=target_row_[target];if(s<0||t<0)return nullptr;
        const unsigned x=unsigned(dx-xmin_),y=unsigned(dy-ymin_);
        if(!dense_||x>=width_||y>=unsigned(ymax_-ymin_+1))return nullptr;
        return values_.data()+(uint64_t(s)*header_.cells+y*width_+x)*header_.targets+unsigned(t);
    }
    bool has_cell(int dx,int dy)const{const unsigned x=unsigned(dx+72),y=unsigned(dy+72);return x<145&&y<145&&grid_[y*145+x]!=65535;}
    int radius()const{return std::max({std::abs(xmin_),std::abs(xmax_),std::abs(ymin_),std::abs(ymax_)});}
    bool dense()const{return dense_;}
    const Header&header()const{return header_;}
    const std::vector<uint16_t>&sources()const{return sources_;}
    const std::vector<uint16_t>&targets()const{return targets_;}
    const std::vector<uint16_t>&values()const{return values_;}
    const std::vector<uint16_t>&grid()const{return grid_;}
    size_t memory_bytes()const{return values_.size()*2+grid_.size()*2+(sources_.size()+targets_.size())*2+sizeof(source_row_)+sizeof(target_row_);}
private:
    [[noreturn]] static void fail(const char*m){throw std::runtime_error(m);}
    Header header_{};std::array<int16_t,496>source_row_,target_row_;
    std::vector<uint16_t>sources_,targets_,grid_,values_;
    int xmin_=0,xmax_=0,ymin_=0,ymax_=0;unsigned width_=0;bool dense_=false;
};
}
