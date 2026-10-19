#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <zlib.h>

namespace srb_v10_light {
// Little-endian container. uint16 bit patterns (including 65535) are lossless.
struct Header {
    char magic[8]; // SRB10LOC or SRB10NAT
    uint32_t version,codec,ports,frames,targets,stored_radius,search_radius;
    uint32_t flags,metadata_bytes,reserved;
    uint64_t values,graph_signature;
};
static_assert(sizeof(Header)==64,"light Atlas header layout");
struct Meta {
    uint16_t source_port,route,loop_cost,direction;
    int16_t loop_dx,loop_dy,xmin,xmax,ymin,ymax;
    uint32_t values;
};
static_assert(sizeof(Meta)==24,"light Atlas metadata layout");
struct Frame { uint32_t compressed_bytes,raw_crc32; };
static_assert(sizeof(Frame)==8,"light Atlas frame layout");

class Storage {
public:
    void load(const std::string&path,const char*magic,uint64_t expected_signature=0,
              uint64_t offset=0,uint64_t container_bytes=0) {
        std::ifstream in(path,std::ios::binary);if(!in)fail("cannot open light Atlas");
        in.seekg(0,std::ios::end);const auto end=in.tellg();if(end<0)fail("light Atlas size unavailable");
        const uint64_t file_bytes=uint64_t(end);
        if(offset>file_bytes||(container_bytes&&container_bytes>file_bytes-offset))fail("light Atlas range outside file");
        const uint64_t available=container_bytes?container_bytes:file_bytes-offset;
        uint64_t consumed=0;
        auto read=[&](void*p,size_t n){if(n>available-consumed)fail("truncated light Atlas range");in.read(static_cast<char*>(p),n);if(!in)fail("truncated light Atlas");consumed+=n;};
        in.seekg(offset,std::ios::beg);read(&header_,sizeof(header_));const auto&h=header_;
        if(std::memcmp(h.magic,magic,8)||h.version!=1||h.codec!=1||h.ports!=496||!h.frames||h.frames>496||!h.targets||h.targets>496||h.flags||h.reserved
          ||h.metadata_bytes!=h.targets*2+h.frames*sizeof(Meta)||h.stored_radius!=72||h.search_radius!=80
          ||(expected_signature&&h.graph_signature!=expected_signature))fail("invalid light Atlas header or graph signature");
        targets_.resize(h.targets);metas_.resize(h.frames);offsets_.resize(h.frames);
        read(targets_.data(),h.targets*2);read(metas_.data(),h.frames*sizeof(Meta));
        source_row_.fill(-1);target_row_.fill(-1);
        for(unsigned i=0;i<h.targets;++i){unsigned p=targets_[i];if(p>=496||target_row_[p]>=0)fail("invalid light target IDs");target_row_[p]=int16_t(i);}
        uint64_t total=0,max_frame=0;
        const bool local=std::memcmp(magic,"SRB10LOC",8)==0;
        for(unsigned i=0;i<h.frames;++i){const auto&m=metas_[i];
            if(m.source_port>=496||source_row_[m.source_port]>=0||m.xmin>m.xmax||m.ymin>m.ymax
              ||m.xmin<-72||m.xmax>72||m.ymin<-72||m.ymax>72)fail("invalid light source IDs or bounds");
            const uint64_t n=uint64_t(width(m))*height(m)*h.targets;
            if(n!=m.values)fail("invalid light frame dimensions");
            if(local){if(m.route!=65535||m.loop_dx||m.loop_dy||m.loop_cost||m.direction||m.xmin!=-24||m.xmax!=24||m.ymin!=-24||m.ymax!=24)fail("invalid Local metadata");}
            else {if(m.route>=160||!m.loop_cost)fail("invalid Native loop metadata");
                const bool north=m.direction=='N',south=m.direction=='S',east=m.direction=='E',west=m.direction=='W';
                if(!(north||south||east||west)||m.loop_dx!=(east?10:west?-10:0)||m.loop_dy!=(north?12:south?-12:0))fail("invalid Native loop vector");}
            source_row_[m.source_port]=int16_t(i);offsets_[i]=total;total+=n;max_frame=std::max(max_frame,n*2);
        }
        if(total!=h.values||total>750000000||total*2+max_frame+compressBound(uLong(max_frame))>1850000000ull)fail("light Atlas values or memory budget invalid");
        values_.resize(size_t(total));std::vector<uint8_t>compressed,planes;
        for(unsigned i=0;i<h.frames;++i){const auto&m=metas_[i];const size_t n=m.values;Frame f{};read(&f,sizeof(f));
            if(!f.compressed_bytes||f.compressed_bytes>compressBound(uLong(n*2)))fail("invalid light compressed frame length");
            compressed.resize(f.compressed_bytes);planes.resize(n*2);read(compressed.data(),compressed.size());
            uLongf bytes=uLongf(planes.size());if(uncompress(planes.data(),&bytes,compressed.data(),uLong(compressed.size()))!=Z_OK||bytes!=planes.size())fail("light decompression failed");
            uint16_t*out=values_.data()+offsets_[i];for(size_t j=0;j<n;++j)out[j]=uint16_t(planes[j])|(uint16_t(planes[n+j])<<8);
            const size_t row=size_t(width(m))*h.targets;
            for(unsigned y=1;y<height(m);++y)for(size_t j=0;j<row;++j)out[y*row+j]=uint16_t(out[y*row+j]+out[(y-1)*row+j]);
            for(unsigned y=0;y<height(m);++y)for(unsigned x=1;x<width(m);++x)for(unsigned t=0;t<h.targets;++t){size_t j=(size_t(y)*width(m)+x)*h.targets+t;out[j]=uint16_t(out[j]+out[j-h.targets]);}
            if(crc32(0,reinterpret_cast<const Bytef*>(out),uInt(n*2))!=f.raw_crc32)fail("light frame CRC mismatch");
        }
        if(consumed!=available)fail("trailing light Atlas bytes");
    }
    const Header&header()const{return header_;}
    const Meta&meta(unsigned frame)const{return metas_.at(frame);}
    const std::vector<Meta>&metadata()const{return metas_;}
    const std::vector<uint16_t>&targets()const{return targets_;}
    const std::vector<uint16_t>&values()const{return values_;}
    int source_row(unsigned port)const{return port<496?source_row_[port]:-1;}
    int target_row(unsigned port)const{return port<496?target_row_[port]:-1;}
    uint16_t query(unsigned frame,unsigned target_port,int dx,int dy)const{
        if(frame>=header_.frames||target_port>=496)return 65535;int t=target_row_[target_port];if(t<0)return 65535;
        const auto&m=metas_[frame];unsigned x=unsigned(dx)-unsigned(int(m.xmin)),y=unsigned(dy)-unsigned(int(m.ymin));
        if(x>=width(m)||y>=height(m))return 65535;
        return values_[offsets_[frame]+(uint64_t(y)*width(m)+x)*header_.targets+unsigned(t)];
    }
    uint16_t prepared(unsigned frame,uint32_t cell,unsigned target_row)const{return values_[offsets_[frame]+uint64_t(cell)*header_.targets+target_row];}
    uint64_t frame_base(unsigned frame)const{return offsets_[frame];}
    uint16_t prepared_offset(uint64_t cell_offset,unsigned target_row)const{return values_[cell_offset+target_row];}
    uint64_t memory_bytes()const{return values_.size()*2+targets_.size()*2+metas_.size()*sizeof(Meta)+offsets_.size()*8+sizeof(source_row_)+sizeof(target_row_);}
    static unsigned width(const Meta&m){return unsigned(int(m.xmax)-m.xmin+1);}
    static unsigned height(const Meta&m){return unsigned(int(m.ymax)-m.ymin+1);}
private:
    [[noreturn]] static void fail(const char*m){throw std::runtime_error(m);}
    Header header_{};std::array<int16_t,496>source_row_,target_row_;
    std::vector<Meta>metas_;std::vector<uint16_t>targets_,values_;std::vector<uint64_t>offsets_;
};

class Local:public Storage {
public:
    explicit Local(const std::string&path,uint64_t signature=0,uint64_t offset=0,uint64_t bytes=0){load(path,signature,offset,bytes);}
    void load(const std::string&path,uint64_t signature=0,uint64_t offset=0,uint64_t bytes=0){Storage::load(path,"SRB10LOC",signature,offset,bytes);}
    // source and target are original PortID after caller endpoint normalization.
    uint16_t get(unsigned input_port,unsigned target_port,int dx,int dy)const{int s=source_row(input_port);return s<0?65535:query(unsigned(s),target_port,dx,dy);}
};
class Native:public Storage {
public:
    explicit Native(const std::string&path,uint64_t signature=0,uint64_t offset=0,uint64_t bytes=0){load(path,signature,offset,bytes);}
    void load(const std::string&path,uint64_t signature=0,uint64_t offset=0,uint64_t bytes=0){Storage::load(path,"SRB10NAT",signature,offset,bytes);}
    // Hub index uses native_hubs.json order. rx/ry are actual signed displacement,
    // not positive mod residues. An out-of-stripe remainder is unavailable.
    uint16_t suffix(unsigned hub_index,unsigned target_port,int rx,int ry)const{return query(hub_index,target_port,rx,ry);}
};
}
