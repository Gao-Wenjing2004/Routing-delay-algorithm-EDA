#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <lzma.h>
#include <zlib.h>

namespace srb_v11_hull {
struct Header {
    char magic[8];uint32_t version,codec,sources,targets,prototypes,width,phase_y,reserved;
    uint64_t curves,tails,signature;
};
static_assert(sizeof(Header)==64,"hull header layout");
struct Frame {uint32_t levels,bytes,crc,tails;};
#pragma pack(push,1)
struct Tail {uint16_t code;uint8_t q;};
#pragma pack(pop)
static_assert(sizeof(Tail)==3,"hull tail layout");

// Complete q6..45 lower envelope of the finite V-native+fixed medoid teacher.
// Physical normalization/gates/extra/gap and frozen residual belong to caller.
// The target medoid/bias remains approximate; this is not the full graph oracle.
class Table {
public:
    explicit Table(const std::string&path,uint64_t expected_signature=0){load(path,expected_signature);}
    void load(const std::string&path,uint64_t expected_signature=0){
        std::ifstream in(path,std::ios::binary);if(!in)fail("hull open failed");
        auto read=[&](void*dest,size_t n){in.read(static_cast<char*>(dest),std::streamsize(n));if(!in)fail("hull truncated");};
        read(&header_,sizeof(header_));const auto&h=header_;
        const size_t N=size_t(189)*12*32;
        if(std::memcmp(h.magic,"SRB11HUL",8)||h.version!=1||h.codec!=1||h.sources<1||h.sources>496||h.targets<1||h.targets>496||h.prototypes!=32||h.width!=189||h.phase_y!=12||h.reserved
            ||h.curves!=uint64_t(2)*h.sources*N||h.curves>100000000||h.tails>h.curves*5||(expected_signature&&h.signature!=expected_signature))fail("hull header invalid");
        std::vector<uint16_t>sources(h.sources),targets(h.targets),medoids(32);
        read(sources.data(),sources.size()*2);read(targets.data(),targets.size()*2);read(medoids.data(),medoids.size()*2);
        assignment_.resize(h.targets);bias_.resize(size_t(2)*12*h.targets);
        read(assignment_.data(),assignment_.size());read(bias_.data(),bias_.size()*2);read(slopes_,sizeof(slopes_));
        sm_.fill(-1);tm_.fill(-1);
        for(unsigned i=0;i<h.sources;++i){unsigned p=sources[i];if(p>=496||sm_[p]>=0)fail("hull source IDs invalid");sm_[p]=int16_t(i);}
        for(unsigned i=0;i<h.targets;++i){unsigned p=targets[i];if(p>=496||tm_[p]>=0)fail("hull target IDs invalid");tm_[p]=int16_t(i);}
        std::array<bool,496>seen{};for(unsigned p:medoids){if(p>=496||tm_[p]<0||seen[p])fail("hull medoids invalid");seen[p]=true;}
        for(unsigned p:assignment_)if(p>=32)fail("hull assignment invalid");
        for(unsigned d=0;d<2;++d){for(unsigned c=0;c<6;++c)if(slopes_[d][c]<100||slopes_[d][c]>200||(c&&slopes_[d][c]<=slopes_[d][c-1]))fail("hull slopes invalid");if(slopes_[d][6]||slopes_[d][7])fail("hull unused slope invalid");}
        first_.resize(size_t(h.curves));offset_bits_.assign((size_t(h.curves)*11+7)/8+3,0);blocks_.resize((size_t(h.curves)+255)/256+1);
        tails_.clear();tails_.reserve(size_t(h.tails));tail_bits_.assign((size_t(h.curves)+7)/8,0);compact_=true;
        for(unsigned frame=0;frame<2*h.sources;++frame){
            Frame f{};read(&f,sizeof(f));if(f.levels<1||f.levels>6||!f.bytes||f.bytes>N*f.levels*2+65536||f.tails>N*(f.levels-1))fail("hull frame invalid");
            const size_t n=N*f.levels;std::vector<uint8_t>compressed(f.bytes),planes(n*2);read(compressed.data(),compressed.size());
            uint64_t limit=64ull<<20;size_t ip=0,op=0;
            if(lzma_stream_buffer_decode(&limit,0,nullptr,compressed.data(),&ip,compressed.size(),planes.data(),&op,planes.size())!=LZMA_OK||ip!=compressed.size()||op!=planes.size())fail("hull XZ decode failed");
            std::vector<uint16_t>fields(n);
            for(size_t i=0;i<n;++i){uint16_t z=planes[i]|uint16_t(planes[n+i])<<8;fields[i]=uint16_t((z>>1)^uint16_t(-int(z&1)));}
            for(unsigned level=0;level<f.levels;++level){uint16_t*p=fields.data()+level*N;
                for(unsigned x=10;x<189;++x)for(size_t j=0;j<12*32;++j){size_t i=size_t(x)*12*32+j;p[i]=uint16_t(p[i]+p[i-10*12*32]);}
                for(unsigned x=0;x<189;++x)for(unsigned y=1;y<12;++y)for(unsigned t=0;t<32;++t){size_t i=(size_t(x)*12+y)*32+t;p[i]=uint16_t(p[i]+p[i-32]);}
            }
            if(crc32(0,reinterpret_cast<const Bytef*>(fields.data()),uInt(n*2))!=f.crc)fail("hull frame CRC differs");
            // Relative zero denotes absence; all later levels must remain absent.
            for(unsigned level=1;level<f.levels;++level)for(size_t i=0;i<N;++i){uint16_t&v=fields[level*N+i];uint16_t prev=fields[(level-1)*N+i];if(v&&prev==65535)fail("hull segment after absence");v=v?uint16_t(prev+v):65535;}
            const unsigned direction=frame/h.sources;size_t frame_start=size_t(frame)*N,tail_start=tails_.size();
            for(size_t i=0;i<N;++i){size_t index=frame_start+i;if(!(index&255))blocks_[index>>8]=uint32_t(tails_.size());
                size_t relative=tails_.size()-blocks_[index>>8];if(relative>=2048)fail("hull local offset overflow");set_offset(index,unsigned(relative));
                uint16_t first=fields[i];first_[index]=first;if(first==65535){for(unsigned l=1;l<f.levels;++l)if(fields[l*N+i]!=65535)fail("hull missing curve has tail");continue;}
                unsigned pm=first>>13;if(pm>=6)fail("hull first mode invalid");int pi=intercept(first);if(pi<-2048||pi>2047)compact_=false;
                unsigned previous_q=6;
                for(unsigned level=1;level<f.levels;++level){uint16_t code=fields[level*N+i];if(code==65535)break;
                    unsigned mode=code>>13;if(mode>=pm||mode>=6)fail("hull slopes not decreasing");int it=intercept(code),dc=int(slopes_[direction][pm])-slopes_[direction][mode],di=it-pi;
                    int q=(di+dc-1)/dc;if(di<=0||q<=int(previous_q)||q>45)fail("hull threshold invalid");
                    if(tails_.size()>=h.tails)fail("hull tails exceed header");tails_.push_back(Tail{code,uint8_t(q)});tail_bits_[index>>3]|=uint8_t(1u<<(index&7));
                    pm=mode;pi=it;previous_q=unsigned(q);
                }
            }
            if(tails_.size()-tail_start!=f.tails)fail("hull frame tail count differs");
        }
        if(tails_.size()!=h.tails||in.peek()!=EOF)fail("hull final size differs");blocks_.back()=uint32_t(tails_.size());
        if(compact_){for(size_t i=0;i<first_.size();++i)if(first_[i]!=65535){uint16_t old=first_[i];bool tail=(tail_bits_[i>>3]>>(i&7))&1;first_[i]=uint16_t((old>>13)<<12)|(uint16_t(intercept(old))&4095)|(tail?32768:0);}
            std::vector<uint8_t>().swap(tail_bits_);
        }
    }
    struct PreparedQuery{size_t index;unsigned q,direction,target;};
    bool prepare(unsigned source,unsigned target,int dx,int dy,PreparedQuery&p)const{
        if(source>=496||target>=496||dx<-94||dx>94)return false;int s=sm_[source],t=tm_[target],ay=dy<0?-dy:dy;
        if(s<0||t<0||ay<72||ay>=552)return false;unsigned direction=dy<0,q=unsigned(ay)/12,ry=unsigned(ay)%12;
        const size_t i=(((size_t(direction)*header_.sources+unsigned(s))*189+unsigned(dx+94))*12+ry)*32+assignment_[t];
        p={i,q,direction,unsigned(t)};return true;
    }
    void prefetch(const PreparedQuery&p)const{
#if defined(__GNUC__) || defined(__clang__)
        __builtin_prefetch(first_.data()+p.index,0,3);
        __builtin_prefetch(offset_bits_.data()+((p.index*11)>>3),0,3);
        __builtin_prefetch(blocks_.data()+(p.index>>8),0,3);
#endif
    }
    bool query(unsigned source,unsigned target,int dx,int dy,uint32_t&out)const{
        PreparedQuery p;return prepare(source,target,dx,dy,p)&&query_prepared(p,out);
    }
    bool query_prepared(const PreparedQuery&p,uint32_t&out)const{
        const size_t i=p.index;const unsigned q=p.q,direction=p.direction,t=p.target;
        const unsigned ry=unsigned(i/32)%12;
        uint16_t first=first_[i];if(first==65535)return false;unsigned mode;int it;bool tail;
        if(compact_){mode=(first>>12)&7;it=first&4095;if(it>=2048)it-=4096;tail=first&32768;}
        else{mode=first>>13;it=intercept(first);tail=(tail_bits_[i>>3]>>(i&7))&1;}
        if(tail){size_t b=i>>8;uint32_t start=blocks_[b]+get_offset(i),end=(i&255)==255?blocks_[b+1]:blocks_[b]+get_offset(i+1);
            for(uint32_t r=start;r<end;++r){const Tail&next=tails_[r];if(next.q>q)break;mode=next.code>>13;it=intercept(next.code);}
        }
        int value=it+int(slopes_[direction][mode])*q+bias_[(direction*12+ry)*header_.targets+unsigned(t)];if(value<0)return false;out=uint32_t(value);return true;
    }
    size_t memory_bytes()const{return first_.size()*2+offset_bits_.size()+blocks_.size()*4+tails_.size()*3+tail_bits_.size()+bias_.size()*2+assignment_.size()+sizeof(sm_)+sizeof(tm_)+sizeof(slopes_);}
    bool compact_first()const{return compact_;}
    const Header&header()const{return header_;}
private:
    static int intercept(uint16_t word){int value=word&8191;return value>=4096?value-8192:value;}
    void set_offset(size_t i,unsigned v){size_t bit=i*11,b=bit>>3;unsigned packed=v<<(bit&7);offset_bits_[b]|=uint8_t(packed);offset_bits_[b+1]|=uint8_t(packed>>8);offset_bits_[b+2]|=uint8_t(packed>>16);}
    unsigned get_offset(size_t i)const{size_t bit=i*11,b=bit>>3;unsigned packed=offset_bits_[b]|(unsigned(offset_bits_[b+1])<<8)|(unsigned(offset_bits_[b+2])<<16);return (packed>>(bit&7))&2047;}
    [[noreturn]]static void fail(const char*message){throw std::runtime_error(message);}
    Header header_{};std::array<int16_t,496>sm_{},tm_{};uint16_t slopes_[2][8]{};bool compact_=false;
    std::vector<uint16_t>first_;std::vector<uint8_t>offset_bits_,tail_bits_,assignment_;std::vector<uint32_t>blocks_;std::vector<Tail>tails_;std::vector<int16_t>bias_;
};
}
