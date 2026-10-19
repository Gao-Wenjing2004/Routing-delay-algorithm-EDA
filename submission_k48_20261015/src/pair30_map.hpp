#pragma once
#include "sha256.hpp" // Existing parent helper; no extra cryptographic runtime.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <lzma.h>
#include <zlib.h>

namespace srb_v11_pair30 {
enum class Deployment { D72, D76 };
inline constexpr uint64_t kGraph=3036222110255734598ull;
inline constexpr uint32_t kAssignmentCRC=846889437u,kC4CRC=1194033363u;
inline constexpr uint32_t kPotentialCRC=640936703u,kCountCRC=2602109886u,kPhiCRC=3486986971u;
inline constexpr const char* kContextSha="ae41e87b3d7c00bbd461c4b61f8e1e37522aa081c15f02d2b967d1ae1ded7914";
inline constexpr const char* kDeltaSha="4f13c19a4cb9c1fc024f35eb08abde462cf8b0b6fe823723063c67c5817807d1";
inline constexpr const char* kDirectSha="c030f548c66aaf9ebef278d7a8e960f5259098a9c14d517f980879153f3fa826";
inline constexpr const char* kD72Sha="fc77e8416fa33df6644356ee343b7ca99f289847c68d951813b1bbea394cada1";
inline constexpr const char* kD76Sha="48950b3b96a05a6ba5c4e19693c821e7290e1475bd3abc05ac16dca328427a72";
inline constexpr const char* kOverlapSha="0825713b90133decd2f18e786101b2888313319afd7c33480bcc1843c157bd3d";
inline constexpr size_t kValues=110u*4u*30u*256u,kSourceRaw=4u*30u*256u;

namespace detail {
[[noreturn]]inline void fail(const char* text){throw std::runtime_error(text);}
inline uint16_t u16(const uint8_t* p){return uint16_t(p[0])|(uint16_t(p[1])<<8);}
inline uint32_t u32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
inline uint64_t u64(const uint8_t* p){return uint64_t(u32(p))|(uint64_t(u32(p+4))<<32);}
inline void read(std::istream& f,void* p,size_t n){f.read(static_cast<char*>(p),std::streamsize(n));if(!f)fail("pair30 truncated bytes");}
inline std::string hex(const uint8_t* bytes,size_t n){
    static constexpr char digits[]="0123456789abcdef";std::string s(n*2,'0');
    for(size_t i=0;i<n;++i){s[2*i]=digits[bytes[i]>>4];s[2*i+1]=digits[bytes[i]&15];}return s;
}
template<class Provider,class PriorBinding>
inline void borrowed_binding(const Provider& provider,const PriorBinding& b,Deployment deployment){
    const auto& legacy=provider.legacy();const auto& h=legacy.header();
    const unsigned geometry=deployment==Deployment::D72?72:76,cells=deployment==Deployment::D72?8112:9240;
    if(h.signature!=kGraph||h.sources!=110||h.targets!=256||h.prototypes!=32||h.radius!=72||h.search!=80||h.cells!=21025
        ||h.geometry!=geometry||legacy.stored_cells()!=cells||b.graph_signature!=kGraph||b.d76_asset_sha256!=kD76Sha
        ||b.potential_crc!=kPotentialCRC||b.count_crc!=kCountCRC||b.assignment_crc!=kC4CRC||b.expanded_bias_crc!=kPhiCRC)
        fail("pair30 original deployment/prior binding");
    if(legacy.sources().size()!=110||legacy.targets().size()!=256||legacy.medoids().size()!=32||legacy.assignment().size()!=256
        ||!std::equal(b.sources.begin(),b.sources.end(),legacy.sources().begin())
        ||!std::equal(b.targets.begin(),b.targets.end(),legacy.targets().begin())
        ||!std::equal(b.medoids.begin(),b.medoids.end(),legacy.medoids().begin())
        ||!std::equal(b.old_assignment.begin(),b.old_assignment.end(),legacy.assignment().begin()))fail("pair30 ordered original provider maps");
    const auto& old=legacy.architecture_bias();const auto& c4=provider.adaptive_assignment();const auto& phi=provider.adaptive_bias();
    if(old.size()!=4*120*256||c4.size()!=110*4*256||phi.size()!=32*4*120*256
        ||crc32(0,reinterpret_cast<const Bytef*>(old.data()),uInt(old.size()*2))!=b.old_bias_crc
        ||crc32(0,c4.data(),uInt(c4.size()))!=kC4CRC
        ||crc32(0,reinterpret_cast<const Bytef*>(phi.data()),uInt(phi.size()*2))!=kPhiCRC)
        fail("pair30 borrowed oldbeta/C4/fullphi CRC");
}

// Structural parser for the constructor and explicitly separate fault-fixture
// audit. This does not grant request guards or waive production SHA binding.
template<class Provider>
inline std::vector<uint8_t> decode_bound_stream(std::istream& f,const Provider& provider,unsigned codec){
    if(codec>1)fail("pair30 codec parameter");
    if(provider.legacy().sources().size()!=110||provider.legacy().targets().size()!=256||provider.legacy().medoids().size()!=32)
        fail("pair30 structural provider dimensions");
    std::array<uint8_t,116> header{};read(f,header.data(),header.size());
    const auto H=[&](unsigned i){return u32(header.data()+8+4*(i-1));};
    if(std::memcmp(header.data(),"SRBPA301",8)||H(1)!=1||H(2)!=codec||H(3)!=110||H(4)!=256||H(5)!=4||H(6)!=30
        ||H(7)!=32||H(8)!=72||H(9)!=80||H(10)!=21025||H(11)!=1||u64(header.data()+76)!=kGraph
        ||hex(header.data()+84,32)!=kContextSha||H(13)!=kAssignmentCRC||H(14)!=kC4CRC||H(15)!=kPotentialCRC||H(16)!=kCountCRC||H(17)!=kPhiCRC)
        fail("pair30 header/dim/domain/graph/context/prior CRC");
    std::array<uint8_t,796> metadata{};read(f,metadata.data(),metadata.size());
    if(crc32(0,metadata.data(),uInt(metadata.size()))!=H(12))fail("pair30 ordered metadata CRC");
    const auto& legacy=provider.legacy();size_t at=0;
    std::array<bool,496> seen{};
    for(auto expected:legacy.sources()){
        unsigned p=u16(metadata.data()+at);at+=2;
        if(p>=496||seen[p]||p!=expected)fail("pair30 source order/range/duplicates");seen[p]=true;
    }
    seen.fill(false);
    for(auto expected:legacy.targets()){
        unsigned p=u16(metadata.data()+at);at+=2;
        if(p>=496||seen[p]||p!=expected)fail("pair30 target order/range/duplicates");seen[p]=true;
    }
    seen.fill(false);
    for(auto expected:legacy.medoids()){
        unsigned p=u16(metadata.data()+at);at+=2;
        if(p>=496||seen[p]||p!=expected)fail("pair30 medoid order/range/duplicates");seen[p]=true;
    }
    if(at!=metadata.size())fail("pair30 metadata dimensions");
    const auto& c4=provider.adaptive_assignment();
    if(c4.size()!=110*4*256||crc32(0,c4.data(),uInt(c4.size()))!=kC4CRC||std::any_of(c4.begin(),c4.end(),[](uint8_t v){return v>=32;}))
        fail("pair30 borrowed C4 decode binding");
    std::vector<uint8_t> map(kValues);std::array<uint8_t,kSourceRaw> encoded{};uint32_t full_crc=0;
    for(unsigned sr=0;sr<110;++sr){
        std::array<uint8_t,16> frame{};read(f,frame.data(),frame.size());
        const uint32_t compressed=u32(frame.data()),raw=u32(frame.data()+4),encoded_crc=u32(frame.data()+8),restored_crc=u32(frame.data()+12);
        if(raw!=kSourceRaw||!compressed||compressed>kSourceRaw+kSourceRaw/3+128)fail("pair30 frame raw/compressed bounds");
        std::vector<uint8_t> bytes(compressed);read(f,bytes.data(),bytes.size());
        uint64_t memlimit=64ull<<20;size_t ip=0,op=0;
        if(lzma_stream_buffer_decode(&memlimit,0,nullptr,bytes.data(),&ip,bytes.size(),encoded.data(),&op,encoded.size())!=LZMA_OK
            ||ip!=bytes.size()||op!=encoded.size())fail("pair30 XZ one-stream/memlimit/length/trailing");
        if(crc32(0,encoded.data(),uInt(encoded.size()))!=encoded_crc)fail("pair30 encoded frame CRC");
        uint8_t* dst=map.data()+size_t(sr)*kSourceRaw;
        for(unsigned q=0;q<4;++q)for(unsigned b=0;b<30;++b)for(unsigned t=0;t<256;++t){
            const size_t pos=(q*30+b)*256+t;const unsigned v=encoded[pos];
            if(v>=32)fail("pair30 encoded medoid/mod32 range");
            dst[pos]=codec==0?uint8_t(v):uint8_t((v+c4[(sr*4+q)*256+t])&31u);
        }
        if(crc32(0,dst,uInt(kSourceRaw))!=restored_crc)fail("pair30 restored frame CRC");
        full_crc=crc32(full_crc,dst,uInt(kSourceRaw));
    }
    if(f.peek()!=EOF)fail("pair30 extra trailing bytes");
    if(full_crc!=kAssignmentCRC)fail("pair30 complete assignment CRC");
    return map;
}
} // namespace detail

class AssignmentMap {
public:
    // Provider and original typed Binding must already have been validated by
    // their immutable constructor. Only the new map persists here; all provider
    // references are borrowed during this constructor, not saved as members.
    template<class Provider,class PriorBinding>
    AssignmentMap(const std::string& sidecar_path,const Provider& provider,const PriorBinding& prior,
                  Deployment deployment,const std::string& actual_cost_path,unsigned codec=1){
        if(codec>1)detail::fail("pair30 codec parameter");
        if(deployment!=Deployment::D72&&deployment!=Deployment::D76)detail::fail("pair30 deployment parameter");
        detail::borrowed_binding(provider,prior,deployment);
        const uintmax_t expected_bytes=codec==1?684272u:688948u;
        if(std::filesystem::file_size(sidecar_path)!=expected_bytes)detail::fail("pair30 actual frozen sidecar size");
        if(srb_v11_adaptive_a::sha256_file(sidecar_path)!=(codec==1?kDeltaSha:kDirectSha))detail::fail("pair30 actual frozen sidecar SHA");
        if(srb_v11_adaptive_a::sha256_file(actual_cost_path)!=(deployment==Deployment::D72?kD72Sha:kD76Sha))
            detail::fail("pair30 actual D72/D76 deployment cost SHA");
        // D72 original typed Binding adds separate deployed-cost/overlap fields;
        // that stronger type check remains the existing provider constructor's
        // responsibility. Sidecar retains immutable D76 parameter provenance.
        std::ifstream input(sidecar_path,std::ios::binary);if(!input)detail::fail("pair30 open failed");
        map_=detail::decode_bound_stream(input,provider,codec);
    }
    unsigned medoid_at(unsigned source_row,unsigned quadrant,unsigned signed_phase,unsigned target_row)const{
        if(source_row>=110||quadrant>=4||signed_phase>=120||target_row>=256)detail::fail("pair30 index bounds");
        const unsigned bucket=((signed_phase/12)/2)*6+(signed_phase%12)/2;
        return map_[((size_t(source_row)*4+quadrant)*30+bucket)*256+target_row];
    }
    const std::vector<uint8_t>& values()const{return map_;}
    size_t owned_payload_bytes()const{return map_.size();}
    size_t owned_capacity_bytes()const{return map_.capacity();} // actual allocation diagnostic, not RSS.
private:
    std::vector<uint8_t> map_;
};
} // namespace srb_v11_pair30
