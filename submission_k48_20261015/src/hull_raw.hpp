#pragma once
#include "binding_types.hpp"
#include "asset_binding.hpp"
#include "sha256.hpp"
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>
#include <lzma.h>
#include <zlib.h>

namespace srb_v11_adaptive_long_hull {
struct Header {
    char magic[8];uint32_t version,codec,sources,targets,prototypes,width,phase_y,reserved;
    uint64_t curves,tails,signature;
};
static_assert(sizeof(Header)==64,"original Hull ABI64");
struct Frame {uint32_t levels,bytes,crc,tails;};
#pragma pack(push,1)
struct Tail {uint16_t code;uint8_t q;};
#pragma pack(pop)
static_assert(sizeof(Tail)==3,"original Tail ABI3");
static_assert(sizeof(int)==4 && sizeof(uint32_t)==4 && sizeof(int16_t)==2,"original MinGW32-bit arithmetic");

class Table {
public:
    static const char* deployed_asset_sha256(){return "a33ef5276d82d85e416e6ab323c581026c96d4af8ee173d9afb7fe4b3cbbc7fe";}
    static const char* original_asset_sha256(){return "66ae4d1187d05e694f7c3ab82949532daf00e864cfb1010828a3af0fbc1ead51";}
    explicit Table(const std::string&path,uint64_t expected_signature=0){load(path,expected_signature);}
    void load(const std::string&path,uint64_t expected_signature=0){
        // Build separately: rejected reloads leave every old packed field intact.
        Table replacement(UnloadedTag{});
        replacement.load_bound(path,expected_signature,srb_v11_normalized_hull_binding::compiled_binding());
        *this=std::move(replacement);
    }
#ifdef SRB_NORMALIZED_TEST_BINDING
    // Standalone malformed-data harness only. Production never exposes rebinding.
    Table(const std::string&path,uint64_t signature,const srb_v11_normalized_hull_binding::Binding&binding){load_bound(path,signature,binding);}
#endif
    struct PreparedQuery{size_t index;unsigned q,direction,target;};
    bool prepare_medoid(unsigned source,unsigned target,int dx,int dy,unsigned medoid,PreparedQuery&p)const{
        if(source>=496||target>=496||medoid>=32||dx<-94||dx>94||dy<=-552||dy>=552)return false;
        const int s=sm_[source],t=tm_[target];const unsigned ay=unsigned(dy<0?-dy:dy);
        if(s<0||t<0||ay<72)return false;
        const unsigned direction=dy<0,q=ay/12,ry=ay%12;
        const size_t i=(((size_t(direction)*header_.sources+unsigned(s))*189+unsigned(dx+94))*12+ry)*32+medoid;
        p={i,q,direction,unsigned(t)};return true;
    }
    bool prepare(unsigned source,unsigned target,int dx,int dy,PreparedQuery&p)const{
        // Additional pre-abs range guard makes INT_MIN defined. Every original
        // well-defined prepared tuple is unchanged; original INT_MIN abs was UB.
        if(source>=496||target>=496||dx<-94||dx>94||dy<=-552||dy>=552)return false;
        const int s=sm_[source],t=tm_[target];const unsigned ay=unsigned(dy<0?-dy:dy);
        if(s<0||t<0||ay<72)return false;
        const unsigned direction=dy<0,q=ay/12,ry=ay%12;
        p={(((size_t(direction)*header_.sources+unsigned(s))*189+unsigned(dx+94))*12+ry)*32+assignment_[t],q,direction,unsigned(t)};
        return true;
    }
    void prefetch(const PreparedQuery&p)const{
#if defined(__GNUC__) || defined(__clang__)
        if(p.index<uid_.size()){
            __builtin_prefetch(uid_.data()+p.index,0,3);
            // Packed word contains the anchor and encoded direction; one record prefetch.
        }
#endif
    }
    bool query(unsigned source,unsigned target,int dx,int dy,uint32_t&out)const{
        PreparedQuery p;return prepare(source,target,dx,dy,p)&&query_prepared(p,out);
    }
    bool query_prepared(const PreparedQuery&p,uint32_t&out)const{
        if(p.index>=uid_.size()||p.direction>=2||p.target>=header_.targets)return false;
        const uint32_t id=uid_[p.index]&262143u;if(!id)return false;
        uint32_t raw;
        if(p.q>=6&&p.q<=45&&encoded_direction(p.index)==p.direction)
            raw=uint32_t(int32_t(signed_anchor(p.index))+cache_[size_t(id)*40+p.q-6]);
        else raw=slow_raw_bits(p,id); // old encoded cuts, passed slope, arbitrary unsigned q
        const unsigned ry=unsigned(p.index/32)%12;
        const uint32_t bits=raw+uint32_t(int32_t(bias_[(p.direction*12+ry)*header_.targets+p.target]));
        if(signed_bits(bits)<0)return false;out=bits;return true;
    }
    bool query_medoid_prepared(const PreparedQuery&p,int32_t&out)const{
        if(p.index>=uid_.size()||p.q<6||p.q>45||p.direction>=2||p.target>=header_.targets)return false;
        const uint32_t id=uid_[p.index]&262143u;if(!id)return false;
        if(encoded_direction(p.index)==p.direction)
            out=int32_t(signed_anchor(p.index))+cache_[size_t(id)*40+p.q-6];
        else out=signed_bits(slow_raw_bits(p,id));
        return true;
    }
    bool curve_present(const PreparedQuery&p)const{return p.index<uid_.size()&&uid_[p.index]!=0;}
    const std::vector<uint16_t>&sources()const{return sources_;}
    const std::vector<uint16_t>&targets()const{return targets_;}
    const std::vector<uint16_t>&medoids()const{return medoids_;}
    const std::vector<uint8_t>&assignment()const{return assignment_;}
    const std::vector<int16_t>&legacy_bias()const{return bias_;}
    int source_row(unsigned port)const{return port<496?sm_[port]:-1;}
    int target_row(unsigned port)const{return port<496?tm_[port]:-1;}
    size_t memory_bytes()const{
        return uid_.capacity()*4+anchor_.capacity()*2+keys_.capacity()+cache_.capacity()*2
            +bias_.capacity()*2+assignment_.capacity()+(sources_.capacity()+targets_.capacity()+medoids_.capacity())*2
            +sizeof(sm_)+sizeof(tm_)+sizeof(slopes_);
    }
    bool compact_first()const{return compact_;}
    const Header&header()const{return header_;}
private:
    struct UnloadedTag{};
    explicit Table(UnloadedTag){}
    static constexpr uint64_t Curves=15966720,FrameCurves=72576,Graph=3036222110255734598ull;
    static constexpr uint64_t MaximumEntries=10000000,MaximumOwnedBytes=1500000000;
    static uint16_t le16(const uint8_t*p){return uint16_t(p[0])|uint16_t(p[1])<<8;}
    static uint32_t le32(const uint8_t*p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
    static uint64_t le64(const uint8_t*p){return uint64_t(le32(p))|uint64_t(le32(p+4))<<32;}
    static int32_t signed_bits(uint32_t bits){int32_t value;std::memcpy(&value,&bits,4);return value;}
    static int16_t signed16(uint16_t bits){int16_t value;std::memcpy(&value,&bits,2);return value;}
    int16_t signed_anchor(size_t i)const{return packed_?int16_t((uid_[i]>>18)&4095u):signed16(anchor_[i]);}
    unsigned encoded_direction(size_t i)const{return packed_?unsigned((uid_[i]>>30)&1u):unsigned(i/(FrameCurves*header_.sources));}
    const uint8_t*key(uint32_t id)const{return keys_.data()+size_t(id)*31;}
    static uint32_t z_at(const uint8_t*k,unsigned level){return le32(k+7+4*level);}
    int32_t line_intercept(size_t index,const uint8_t*k,unsigned level)const{
        return int32_t(int64_t(signed_anchor(index))+z_at(k,level)-6*int64_t(slopes_[k[0]][k[1+level]]));
    }
    uint32_t slow_raw_bits(const PreparedQuery&p,uint32_t id)const{
        const uint8_t*k=key(id);unsigned mode=k[1];int32_t intercept=line_intercept(p.index,k,0);
        unsigned previous_mode=mode;int32_t previous_intercept=intercept;
        for(unsigned level=1;level<6&&k[1+level]!=255;++level){
            const unsigned next_mode=k[1+level];const int32_t next_intercept=line_intercept(p.index,k,level);
            const int32_t dc=int32_t(slopes_[k[0]][previous_mode])-slopes_[k[0]][next_mode];
            const unsigned cut=unsigned((next_intercept-previous_intercept+dc-1)/dc);
            if(cut>p.q)break;mode=next_mode;intercept=next_intercept;
            previous_mode=next_mode;previous_intercept=next_intercept;
        }
        // Original arithmetic has unsigned q: addition/multiplication wrap u32,
        // then original MinGW converts to32-bit signed for the negative gate.
        return uint32_t(intercept)+uint32_t(slopes_[p.direction][mode])*uint32_t(p.q);
    }
    static void exact_read(std::ifstream&stream,void*output,size_t size){
        stream.read(static_cast<char*>(output),std::streamsize(size));if(!stream)fail("normalized truncated");
    }
    static void validate_binding(const srb_v11_normalized_hull_binding::Binding&b){
        if(b.asset_bytes!=16097406||b.header_bytes!=15764||b.dictionary_entries<1||b.dictionary_entries>MaximumEntries
                ||b.dictionary_entries>262144u||b.uid_width!=(b.dictionary_entries<=65536?2u:3u))fail("normalized preallocation binding dimensions");
        const uint64_t D=b.dictionary_entries;
        const uint64_t products[7]={13436,3520,120570,31*D,Curves*b.uid_width,Curves*2,Curves/8};
        uint64_t position=b.header_bytes;
        for(unsigned j=0;j<7;++j){const auto&s=b.sections[j];
            if(s.offset!=position||s.raw_bytes!=products[j]||!s.bytes||s.bytes>(j<3?s.raw_bytes:s.raw_bytes+65536)
                    ||(j<3&&s.bytes!=s.raw_bytes)||position>b.asset_bytes||s.bytes>b.asset_bytes-position)
                fail("normalized exact7 preallocation products/lengths");
            position+=s.bytes;
        }
        if(position!=b.asset_bytes||111*D+Curves*6>MaximumOwnedBytes)fail("normalized aggregate/RAM preallocation cap");
    }
    static std::vector<uint8_t> raw_section(std::ifstream&in,const srb_v11_normalized_hull_binding::Section&s){
        if(s.bytes!=s.raw_bytes||s.bytes>1048576)fail("normalized bounded raw section");
        std::vector<uint8_t>raw(size_t(s.bytes));in.seekg(std::streamoff(s.offset));exact_read(in,raw.data(),raw.size());
        srb_v11_adaptive_a::Sha256 digest;digest.update(raw.data(),raw.size());
        if(uint32_t(crc32(0,raw.data(),uInt(raw.size())))!=s.raw_crc||s.raw_crc!=s.compressed_crc
                ||digest.finish()!=s.raw_sha||std::string(s.raw_sha)!=s.compressed_sha)fail("normalized rawsection CRC/SHA");
        return raw;
    }
    template<class Emit> static void xz_section(std::ifstream&in,const srb_v11_normalized_hull_binding::Section&s,Emit emit){
        in.clear();in.seekg(std::streamoff(s.offset));lzma_stream stream=LZMA_STREAM_INIT;
        if(lzma_stream_decoder(&stream,64ull<<20,0)!=LZMA_OK)fail("normalized XZ memlimit init");
        uint64_t remaining=s.bytes,total=0;uint32_t raw_crc=0,compressed_crc=0;
        srb_v11_adaptive_a::Sha256 raw_sha,compressed_sha;
        std::array<uint8_t,65536>input{},output{};
        try{
            while(true){
                if(!stream.avail_in&&remaining){const size_t n=size_t(std::min<uint64_t>(remaining,input.size()));
                    exact_read(in,input.data(),n);remaining-=n;compressed_crc=uint32_t(crc32(compressed_crc,input.data(),uInt(n)));
                    compressed_sha.update(input.data(),n);stream.next_in=input.data();stream.avail_in=n;}
                stream.next_out=output.data();stream.avail_out=output.size();
                const lzma_ret result=lzma_code(&stream,remaining?LZMA_RUN:LZMA_FINISH);
                const size_t made=output.size()-stream.avail_out;
                if(total>s.raw_bytes||made>s.raw_bytes-total)fail("normalized XZ oversized rawproduct");
                raw_crc=uint32_t(crc32(raw_crc,output.data(),uInt(made)));raw_sha.update(output.data(),made);emit(total,output.data(),made);total+=made;
                if(result==LZMA_STREAM_END){if(remaining||stream.avail_in||total!=s.raw_bytes)fail("normalized XZ trailing/multistream/length");break;}
                if(result!=LZMA_OK||(made==0&&stream.avail_in==0&&remaining==0))fail("normalized XZ failure/truncation");
            }
            if(raw_crc!=s.raw_crc||compressed_crc!=s.compressed_crc||raw_sha.finish()!=s.raw_sha||compressed_sha.finish()!=s.compressed_sha)
                fail("normalized XZ compressed/raw CRC/SHA");
        }catch(...){lzma_end(&stream);throw;}
        lzma_end(&stream);
    }
    static bool greater_numeric_key(const uint8_t*current,const uint8_t*previous){
        for(unsigned j=0;j<7;++j)if(current[j]!=previous[j])return current[j]>previous[j];
        for(unsigned j=0;j<6;++j){const uint32_t a=z_at(current,j),b=z_at(previous,j);if(a!=b)return a>b;}
        return false;
    }
    void validate_dictionary(uint32_t D){
        for(uint32_t row=0;row<D;++row){const uint8_t*k=key(row);
            if(!row){for(unsigned j=0;j<7;++j)if(k[j]!=255)fail("normalized reservedmissing key");for(unsigned j=7;j<31;++j)if(k[j])fail("normalized reservedmissing delta");
                for(unsigned q=0;q<40;++q)cache_[q]=65535;continue;}
            if(k[0]>=2||k[1]>=6||(row>1&&!greater_numeric_key(k,key(row-1))))fail("normalized dictionary signature/order");
            bool padding=false;unsigned previous_mode=6,previous_cut=6;
            for(unsigned level=0;level<6;++level){const unsigned mode=k[1+level];
                if(mode==255){padding=true;if(z_at(k,level))fail("normalized missingpadding delta");continue;}
                if(padding||mode>=previous_mode||mode>=6)fail("normalized mode order/padding");
                if(level){const int64_t di=int64_t(z_at(k,level))-z_at(k,level-1)-6*(int64_t(slopes_[k[0]][mode])-slopes_[k[0]][previous_mode]);
                    const int64_t dc=int64_t(slopes_[k[0]][previous_mode])-slopes_[k[0]][mode];
                    if(dc<=0||di<=0)fail("normalized reconstructed cut slope/intercept");
                    const int64_t cut=(di+dc-1)/dc;if(cut<=previous_cut||cut>45)fail("normalized reconstructed cut order");previous_cut=unsigned(cut);}
                previous_mode=mode;
            }
            for(unsigned column=0;column<40;++column){uint64_t minimum=std::numeric_limits<uint64_t>::max();
                for(unsigned level=0;level<6&&k[1+level]!=255;++level)
                    minimum=std::min(minimum,uint64_t(z_at(k,level))+uint64_t(column)*slopes_[k[0]][k[1+level]]);
                if(minimum>7800||(column==0&&minimum))fail("normalized exactmin/u16 cache bounds");
                cache_[size_t(row)*40+column]=uint16_t(minimum);
            }
        }
    }
    template<class Word> static void inverse_spatial(std::vector<Word>&values,uint32_t mask){
        for(size_t i=0;i<values.size();++i){const uint32_t z=uint32_t(values[i]);values[i]=Word(((z>>1)^(z&1?mask:0))&mask);}
        for(size_t frame=0;frame<220;++frame){const size_t base=frame*FrameCurves;
            for(unsigned x=0;x<189;++x)for(unsigned y=1;y<12;++y)for(unsigned m=0;m<32;++m){const size_t i=base+(size_t(x)*12+y)*32+m;values[i]=Word((uint32_t(values[i])+values[i-32])&mask);}
            for(unsigned x=10;x<189;++x)for(size_t j=0;j<12*32;++j){const size_t i=base+size_t(x)*12*32+j;values[i]=Word((uint32_t(values[i])+values[i-3840])&mask);}
        }
    }
    void load_bound(const std::string&path,uint64_t expected_signature,const srb_v11_normalized_hull_binding::Binding&b){
        packed_=false;validate_binding(b);std::ifstream in(path,std::ios::binary);if(!in)fail("normalized open");
        in.seekg(0,std::ios::end);if(in.tellg()!=std::streamoff(b.asset_bytes))fail("normalized exactfile length");
        // Whole SHA precedes all asset-sized allocations. Header only15KiB.
        if(srb_v11_adaptive_a::sha256_file(path)!=b.asset_sha)fail("normalized preallocation wholeSHA");
        in.seekg(0);std::array<uint8_t,16>prefix{};exact_read(in,prefix.data(),prefix.size());
        if(std::memcmp(prefix.data(),"SRBK32F1",8)||le32(prefix.data()+8)!=1||le32(prefix.data()+12)!=b.header_bytes-16)
            fail("normalized fixed prefix/headerlength");
        std::vector<uint8_t>json(size_t(b.header_bytes-16));exact_read(in,json.data(),json.size());
        if(uint32_t(crc32(0,json.data(),uInt(json.size())))!=b.json_crc)fail("normalized immutable JSONheaderCRC");
        const auto metadata=raw_section(in,b.sections[0]);const auto frame_headers=raw_section(in,b.sections[1]);
        raw_section(in,b.sections[2]); // audit rawCRC/SHA checked; trusted BUILD parsed JSON
        std::memcpy(header_.magic,metadata.data(),8);
        // Assign explicit fields instead of type-punning an unaligned metadata struct.
        header_.version=le32(metadata.data()+8);header_.codec=le32(metadata.data()+12);
        header_.sources=le32(metadata.data()+16);header_.targets=le32(metadata.data()+20);
        header_.prototypes=le32(metadata.data()+24);header_.width=le32(metadata.data()+28);
        header_.phase_y=le32(metadata.data()+32);header_.reserved=le32(metadata.data()+36);
        header_.curves=le64(metadata.data()+40);header_.tails=le64(metadata.data()+48);header_.signature=le64(metadata.data()+56);
        if(std::memcmp(header_.magic,"SRB11HUL",8)||header_.version!=1||header_.codec!=1||header_.sources!=110||header_.targets!=256
                ||header_.prototypes!=32||header_.width!=189||header_.phase_y!=12||header_.reserved||header_.curves!=Curves
                ||header_.tails>Curves*5||header_.signature!=Graph||(expected_signature&&header_.signature!=expected_signature))fail("normalized original Header binding");
        sources_.resize(110);targets_.resize(256);medoids_.resize(32);assignment_.resize(256);bias_.resize(6144);
        size_t position=64;
        for(auto&value:sources_){value=le16(metadata.data()+position);position+=2;}
        for(auto&value:targets_){value=le16(metadata.data()+position);position+=2;}
        for(auto&value:medoids_){value=le16(metadata.data()+position);position+=2;}
        for(auto&value:assignment_)value=metadata[position++];
        const size_t bias_at=position;for(auto&value:bias_){value=signed16(le16(metadata.data()+position));position+=2;}
        for(auto&direction:slopes_)for(auto&value:direction){value=le16(metadata.data()+position);position+=2;}
        if(position!=metadata.size()||uint32_t(crc32(0,metadata.data()+64,uInt(bias_at-64)))!=2518757483u
                ||uint32_t(crc32(0,metadata.data()+bias_at,12288))!=2393794328u)fail("normalized original maps/bias CRC");
        sm_.fill(-1);tm_.fill(-1);for(unsigned j=0;j<110;++j){const unsigned port=sources_[j];if(port>=496||sm_[port]>=0)fail("normalized source map");sm_[port]=int16_t(j);}
        for(unsigned j=0;j<256;++j){const unsigned port=targets_[j];if(port>=496||tm_[port]>=0)fail("normalized target map");tm_[port]=int16_t(j);}
        std::array<bool,496>seen{};for(unsigned port:medoids_){if(port>=496||tm_[port]<0||seen[port])fail("normalized medoid map");seen[port]=true;}
        for(unsigned medoid:assignment_)if(medoid>=32)fail("normalized oldassignment");
        for(unsigned d=0;d<2;++d){for(unsigned m=0;m<6;++m)if(slopes_[d][m]<100||slopes_[d][m]>200||(m&&slopes_[d][m]<=slopes_[d][m-1]))fail("normalized slope signature");if(slopes_[d][6]||slopes_[d][7])fail("normalized unused slopes");}
        uint64_t expected_tails=0;for(unsigned j=0;j<220;++j){const uint8_t*f=frame_headers.data()+16*j;const uint32_t levels=le32(f),bytes=le32(f+4),tails=le32(f+12);
            if(levels<1||levels>6||!bytes||bytes>FrameCurves*levels*2+65536||tails>FrameCurves*(levels-1))fail("normalized original frameheader bounds");expected_tails+=tails;}
        if(expected_tails!=header_.tails)fail("normalized original tail sum");
        const uint32_t D=b.dictionary_entries;keys_.assign(size_t(D)*31,0);cache_.resize(size_t(D)*40);
        xz_section(in,b.sections[3],[&](uint64_t at,const uint8_t*data,size_t n){for(size_t j=0;j<n;++j){uint64_t p=at+j;
            if(p<D)keys_[size_t(p)*31]=data[j];else if(p<7ull*D){p-=D;keys_[size_t(p/6)*31+1+size_t(p%6)]=data[j];}
            else{p-=7ull*D;const unsigned byte=unsigned(p/(6ull*D));p%=6ull*D;keys_[size_t(p/6)*31+7+4*size_t(p%6)+byte]=data[j];}}});
        validate_dictionary(D);
        uid_.assign(size_t(Curves),0);anchor_.assign(size_t(Curves),0);
        xz_section(in,b.sections[4],[&](uint64_t at,const uint8_t*data,size_t n){for(size_t j=0;j<n;++j){const uint64_t p=at+j;uid_[size_t(p%Curves)]|=uint32_t(data[j])<<(8*unsigned(p/Curves));}});
        xz_section(in,b.sections[5],[&](uint64_t at,const uint8_t*data,size_t n){for(size_t j=0;j<n;++j){const uint64_t p=at+j;anchor_[size_t(p%Curves)]|=uint16_t(uint16_t(data[j])<<(8*unsigned(p/Curves)));}});
        inverse_spatial(uid_,b.uid_width==2?65535u:16777215u);inverse_spatial(anchor_,65535);
        for(size_t i=0;i<uid_.size();++i)if(uid_[i]>=D)fail("normalized geometry UID bound");
        xz_section(in,b.sections[6],[&](uint64_t at,const uint8_t*data,size_t n){for(size_t j=0;j<n;++j)for(unsigned bit=0;bit<8;++bit){const size_t i=size_t(at+j)*8+bit;
            if(bool((data[j]>>bit)&1)!=bool(uid_[i]==0)||(uid_[i]==0&&anchor_[i]))fail("normalized full missingmask/zeroanchor");}});
        uint64_t actual_tails=0;compact_=true;
        for(size_t i=0;i<uid_.size();++i){const uint32_t id=uid_[i];if(!id)continue;const uint8_t*k=key(id);
            if(k[0]!=encoded_direction(i))fail("normalized geometry direction signature");
            for(unsigned level=0;level<6&&k[1+level]!=255;++level){const int64_t intercept=int64_t(signed_anchor(i))+z_at(k,level)-6*int64_t(slopes_[k[0]][k[1+level]]);
                if(intercept<-4096||intercept>4095)fail("normalized restored signedI13 bound");
                if(!level){if(intercept<-2048||intercept>2047)compact_=false;}else ++actual_tails;
            }
        }
        if(actual_tails!=header_.tails)fail("normalized restored complete tail count");
        // Every original field has been restored/validated before repacking in place.
        for(size_t i=0;i<uid_.size();++i){const uint32_t id=uid_[i];
            if(id>262143u||anchor_[i]>4095u)fail("packed32 original UID/unsigned12 anchor guard");
            uid_[i]=id?(id|(uint32_t(anchor_[i])<<18)|(uint32_t(i/(FrameCurves*header_.sources))<<30)):0u;
        }
        packed_=true;std::vector<uint16_t>().swap(anchor_);
    }
    [[noreturn]]static void fail(const char*message){throw std::runtime_error(message);}
    Header header_{};std::array<int16_t,496>sm_{},tm_{};uint16_t slopes_[2][8]{};bool compact_=false,packed_=false;
    std::vector<uint32_t>uid_;std::vector<uint16_t>anchor_,cache_;std::vector<uint8_t>keys_,assignment_;
    std::vector<int16_t>bias_;std::vector<uint16_t>sources_,targets_,medoids_;
};
}
