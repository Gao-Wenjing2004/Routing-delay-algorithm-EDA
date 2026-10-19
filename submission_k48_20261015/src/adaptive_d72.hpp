#pragma once
#include "short_table.hpp"
#include "sha256.hpp"
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <sstream>

namespace srb_v11_adaptive_d72 {
// Filled only by the separately released binding exporter from complete verified
// full110 outputs and frozen maps. Runtime does not trust an editable JSON file.
struct Binding {
    std::array<uint16_t,110> sources;
    std::array<uint16_t,256> targets;
    std::array<uint16_t,32> medoids;
    std::array<uint8_t,256> old_assignment;
    uint32_t old_bias_crc,potential_crc,count_crc,assignment_crc,expanded_bias_crc;
    uint64_t graph_signature;
    std::string d76_asset_sha256;
    std::string saved_medoid_npy_sha256; // Original D76 parameter provenance.
    std::string deployed_d72_sha256,overlap_proof_sha256; // Separate immutable deployment binding.
};
inline constexpr uint64_t kGraphSignature=3036222110255734598ull;
inline constexpr const char* kD76Sha="48950b3b96a05a6ba5c4e19693c821e7290e1475bd3abc05ac16dca328427a72";
inline constexpr const char* kMedoidNpySha="8a1ab8b4b90c8f89175ceebc52abbe75d799cbcd4fad63a2bfbfee741752f255";

inline constexpr const char* kD72Sha="fc77e8416fa33df6644356ee343b7ca99f289847c68d951813b1bbea394cada1";
inline constexpr const char* kOverlapProofSha="0825713b90133decd2f18e786101b2888313319afd7c33480bcc1843c157bd3d";

inline unsigned phase(int dx,int dy){return unsigned((dx%10+10)%10)*12+unsigned((dy%12+12)%12);}
inline unsigned quadrant(int dx,int dy){return unsigned(dx<0)+2*unsigned(dy<0);}
inline int64_t round_even(int64_t numerator,uint32_t denominator){
    if(!denominator)throw std::runtime_error("adaptive zero rounding denominator");
    // Difference of two int32 potentials is within [-4294967295,4294967295].
    if(numerator<-4294967295ll||numerator>4294967295ll)throw std::runtime_error("adaptive numerator overflow");
    const bool neg=numerator<0;const uint64_t magnitude=uint64_t(neg?-numerator:numerator);
    uint64_t result=magnitude/denominator,remain=magnitude%denominator;
    if(2*remain>denominator||(2*remain==denominator&&(result&1)))++result;
    return neg?-int64_t(result):int64_t(result);
}

struct ContainerHeader{char magic[8];uint32_t version,codec,ndim,crc,raw_bytes,xz_bytes;};
static_assert(sizeof(ContainerHeader)==32,"adaptive container header32");
inline std::vector<uint8_t> decode_container_blob(const std::vector<uint8_t>& container,uint32_t codec,
        std::initializer_list<uint32_t> expected_shape,unsigned width,uint32_t expected_crc){
    std::string bytes(reinterpret_cast<const char*>(container.data()),container.size());
    std::istringstream f(bytes,std::ios::in|std::ios::binary);
    auto read=[&](void* p,size_t n){f.read(static_cast<char*>(p),std::streamsize(n));if(!f)throw std::runtime_error("adaptive container truncated");};
    ContainerHeader h{};read(&h,sizeof(h));
    uint64_t count=1;for(unsigned dim:expected_shape){if(!dim||count>8000000ull/dim)throw std::runtime_error("adaptive dimensions overflow");count*=dim;}
    const uint64_t raw=count*width;
    if(std::memcmp(h.magic,"SRBAPC1\0",8)||h.version!=1||h.codec!=codec||h.ndim!=expected_shape.size()
       ||h.crc!=expected_crc||raw>8000000||h.raw_bytes!=raw||!h.xz_bytes
       ||h.xz_bytes>lzma_stream_buffer_bound(size_t(raw)))throw std::runtime_error("adaptive container header/dim/CRC/length");
    for(unsigned dim:expected_shape){uint32_t actual;read(&actual,4);if(actual!=dim)throw std::runtime_error("adaptive container shape/order");}
    std::vector<uint8_t> compressed(h.xz_bytes),planes(h.raw_bytes),decoded(h.raw_bytes);
    read(compressed.data(),compressed.size());if(f.peek()!=EOF)throw std::runtime_error("adaptive container trailing bytes");
    uint64_t memory_limit=64ull<<20;size_t inpos=0,outpos=0;
    if(lzma_stream_buffer_decode(&memory_limit,0,nullptr,compressed.data(),&inpos,compressed.size(),
       planes.data(),&outpos,planes.size())!=LZMA_OK||inpos!=compressed.size()||outpos!=planes.size())
        throw std::runtime_error("adaptive XZ decode length/memory/trailing");
    // Planes are unsigned low-to-high bytes; reconstruct exact little-endian raw.
    for(size_t i=0;i<count;++i)for(unsigned byte=0;byte<width;++byte)decoded[i*width+byte]=planes[byte*count+i];
    if(crc32(0,decoded.data(),uInt(decoded.size()))!=expected_crc)throw std::runtime_error("adaptive raw CRC");
    return decoded;
}
struct PackHeader{
    char magic[8];
    uint32_t version,ports,sources,targets,medoids,quadrants,phases,radius,search,cells,geometry,streams;
    uint64_t graph_signature;
    uint8_t d76_sha256[32];
    uint32_t old_bias_crc,expanded_bias_crc,potential_bytes,count_bytes,assignment_bytes,maps_crc;
};
static_assert(sizeof(PackHeader)==120,"adaptive packed envelope header120");
inline std::string digest_hex(const uint8_t* p){
    static constexpr char hex[]="0123456789abcdef";std::string out;out.reserve(64);
    for(unsigned i=0;i<32;++i){out.push_back(hex[p[i]>>4]);out.push_back(hex[p[i]&15]);}return out;
}
inline int32_t read_i32_le(const uint8_t* p){
    const uint32_t v=uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
    const int64_t signed_value=v>=0x80000000u?int64_t(v)-4294967296ll:int64_t(v);
    return int32_t(signed_value);
}
inline uint16_t read_u16_le(const uint8_t* p){return uint16_t(p[0])|(uint16_t(p[1])<<8);}

class Table {
public:
    using PreparedQuery=srb_v11_short_k32::Table::PreparedQuery;
    Table(const std::string& cost_path,const std::string& dual_path,const std::string& sidecar_path,
            const Binding& expected,bool force_slow_finite_checks=false)
        :legacy_(cost_path,dual_path,kGraphSignature){
        const auto& h=legacy_.header();
        if(expected.graph_signature!=kGraphSignature||expected.d76_asset_sha256!=kD76Sha
          ||expected.saved_medoid_npy_sha256!=kMedoidNpySha||expected.deployed_d72_sha256!=kD72Sha
          ||expected.overlap_proof_sha256!=kOverlapProofSha||h.signature!=kGraphSignature||h.sources!=110||h.targets!=256
          ||h.prototypes!=32||h.geometry!=72||h.radius!=72||h.search!=80||h.cells!=21025||legacy_.stored_cells()!=8112)
            fail("adaptive deployed D72 header/graph/overlap binding");
        if(srb_v11_adaptive_a::sha256_file(cost_path)!=expected.deployed_d72_sha256)fail("adaptive D72 actual asset SHA binding");
        if(!std::equal(expected.sources.begin(),expected.sources.end(),legacy_.sources().begin())
          ||!std::equal(expected.targets.begin(),expected.targets.end(),legacy_.targets().begin())
          ||!std::equal(expected.medoids.begin(),expected.medoids.end(),legacy_.medoids().begin())
          ||!std::equal(expected.old_assignment.begin(),expected.old_assignment.end(),legacy_.assignment().begin()))
            fail("adaptive source/target/medoid/oldassignment order binding");
        const auto& old_bias=legacy_.architecture_bias();
        if(old_bias.size()!=4*120*256||crc32(0,reinterpret_cast<const Bytef*>(old_bias.data()),uInt(old_bias.size()*2))!=expected.old_bias_crc)
            fail("adaptive old beta binding");
        source_row_.fill(-1);target_row_.fill(-1);
        for(unsigned i=0;i<110;++i)source_row_[expected.sources[i]]=int16_t(i);
        for(unsigned i=0;i<256;++i)target_row_[expected.targets[i]]=int16_t(i);
        std::array<unsigned,32> medoid_target_row{};
        for(unsigned m=0;m<32;++m){const int row=target_row_[expected.medoids[m]];if(row<0)fail("adaptive medoid target row");medoid_target_row[m]=unsigned(row);}
        std::ifstream pack(sidecar_path,std::ios::binary);if(!pack)fail("adaptive pack open");
        auto read=[&](void* p,size_t n){pack.read(static_cast<char*>(p),std::streamsize(n));if(!pack)fail("adaptive pack truncated");};
        PackHeader ph{};read(&ph,sizeof(ph));
        if(std::memcmp(ph.magic,"SRBAA001",8)||ph.version!=1||ph.ports!=496||ph.sources!=110||ph.targets!=256
          ||ph.medoids!=32||ph.quadrants!=4||ph.phases!=120||ph.radius!=72||ph.search!=80||ph.cells!=21025||ph.geometry!=76||ph.streams!=3
          ||ph.graph_signature!=kGraphSignature||digest_hex(ph.d76_sha256)!=expected.d76_asset_sha256
          ||ph.old_bias_crc!=expected.old_bias_crc||ph.expanded_bias_crc!=expected.expanded_bias_crc)
            fail("adaptive pack header/version/dim/graph/cost/CRC binding");
        // Metadata is serialized LE source/target/medoid IDs, then oldassignment.
        std::array<uint8_t,1052> metadata{};read(metadata.data(),metadata.size());
        if(crc32(0,metadata.data(),uInt(metadata.size()))!=ph.maps_crc)fail("adaptive pack map CRC");
        size_t at=0;
        for(unsigned v:expected.sources){if(read_u16_le(metadata.data()+at)!=v)fail("adaptive pack source order");at+=2;}
        for(unsigned v:expected.targets){if(read_u16_le(metadata.data()+at)!=v)fail("adaptive pack target order");at+=2;}
        for(unsigned v:expected.medoids){if(read_u16_le(metadata.data()+at)!=v)fail("adaptive pack medoid order");at+=2;}
        for(unsigned v:expected.old_assignment){if(metadata[at++]!=v)fail("adaptive pack oldassignment order");}
        const std::array<uint32_t,3> lengths{{ph.potential_bytes,ph.count_bytes,ph.assignment_bytes}};
        const std::array<size_t,3> raws{{491520,960,112640}};
        uint64_t total=0;std::array<std::vector<uint8_t>,3> containers;
        for(unsigned i=0;i<3;++i){
            const size_t headerbytes=i==1?40:44;
            if(lengths[i]<=headerbytes||uint64_t(lengths[i])>headerbytes+lzma_stream_buffer_bound(raws[i]))fail("adaptive pack stream length");
            total+=lengths[i];if(total>2000000)fail("adaptive pack aggregate length");
            containers[i].resize(lengths[i]);read(containers[i].data(),containers[i].size());
        }
        if(pack.peek()!=EOF)fail("adaptive pack trailing bytes");
        const auto raw_p=decode_container_blob(containers[0],3,{4,120,256},4,expected.potential_crc);
        const auto raw_n=decode_container_blob(containers[1],4,{4,120},2,expected.count_crc);
        assignment_=decode_container_blob(containers[2],2,{110,4,256},1,expected.assignment_crc);
        for(unsigned v:assignment_)if(v>=32)fail("adaptive assignment medoid range");
        std::vector<int32_t> potential(4*120*256);std::array<uint16_t,480> count{};
        for(size_t i=0;i<potential.size();++i)potential[i]=read_i32_le(raw_p.data()+4*i);
        for(size_t i=0;i<count.size();++i)count[i]=read_u16_le(raw_n.data()+2*i);
        bias_.resize(32*4*120*256);
        for(unsigned m=0;m<32;++m)for(unsigned group=0;group<480;++group)for(unsigned t=0;t<256;++t){
            int64_t value=0;const uint16_t n=count[group];
            if(n<2){if(legacy_.assignment()[t]==m)value=old_bias[group*256+t];}
            else value=round_even(int64_t(potential[group*256+t])-potential[group*256+medoid_target_row[m]],n);
            if(value<-32768||value>32767)fail("adaptive expanded beta int16 overflow");
            bias_[(m*480+group)*256+t]=int16_t(value);
        }
        if(crc32(0,reinterpret_cast<const Bytef*>(bias_.data()),uInt(bias_.size()*2))!=expected.expanded_bias_crc)
            fail("adaptive complete expanded beta CRC");
        // Do not infer stored-mask availability from fit statistics. Verify the
        // actual runtime values; only this complete proof permits omitted loads.
        all_stored_finite_=std::all_of(legacy_.values().begin(),legacy_.values().end(),[](uint16_t v){return v!=65535;});
        finite_fastpath_=all_stored_finite_&&!force_slow_finite_checks;
    }
    bool prepare_adaptive(unsigned source,unsigned target,int dx,int dy,bool guarded_old_A_eligible,PreparedQuery& q)const{
        if(!legacy_.prepare(source,target,dx,dy,q))return false;
        // Eligibility comes only from safe + original!long + oldA caller guards.
        if(!guarded_old_A_eligible||(std::abs(dx)<=26&&std::abs(dy)<=26))return true;
        // Preserve old availability even if a different medoid is finite.
        if(!finite_fastpath_&&*q.value==65535)return true;
        const unsigned s=unsigned(source_row_[source]),t=unsigned(target_row_[target]);
        const unsigned quad=quadrant(dx,dy),ph=phase(dx,dy);
        const unsigned m=assignment_[(s*4+quad)*256+t];
        const uint16_t* chosen=q.value-legacy_.assignment()[t]+m;
        if(!finite_fastpath_&&*chosen==65535)return true;
        q.value=chosen;q.bias=bias_[((m*4+quad)*120+ph)*256+t];return true;
    }
    // Every legacy entry remains legacy. Only explicit prepare_adaptive changes A.
    bool prepare(unsigned s,unsigned t,int dx,int dy,PreparedQuery& q)const{return legacy_.prepare(s,t,dx,dy,q);}
    bool query_prepared(const PreparedQuery& q,uint32_t& out)const{return legacy_.query_prepared(q,out);}
    bool query(unsigned s,unsigned t,int dx,int dy,uint32_t& out)const{return legacy_.query(s,t,dx,dy,out);}
    bool query_adaptive(unsigned s,unsigned t,int dx,int dy,bool eligible,uint32_t& out)const{
        PreparedQuery q{};return prepare_adaptive(s,t,dx,dy,eligible,q)&&query_prepared(q,out);
    }
    bool dualget(unsigned s,unsigned t,int dx,int dy,uint32_t& out)const{return legacy_.dualget(s,t,dx,dy,out);}
    void prefetch(const PreparedQuery& q)const{legacy_.prefetch(q);}
    const srb_v11_short_k32::Table& legacy()const{return legacy_;}
    const std::vector<int16_t>& adaptive_bias()const{return bias_;}
    const std::vector<uint8_t>& adaptive_assignment()const{return assignment_;}
    bool all_stored_finite()const{return all_stored_finite_;}
    bool finite_fastpath_enabled()const{return finite_fastpath_;}
    size_t memory_bytes()const{return legacy_.memory_bytes()+bias_.size()*2+assignment_.size()+sizeof(source_row_)+sizeof(target_row_);}
private:
    [[noreturn]]static void fail(const char* s){throw std::runtime_error(s);}
    srb_v11_short_k32::Table legacy_;
    std::array<int16_t,496> source_row_{},target_row_{};
    std::vector<int16_t> bias_;
    std::vector<uint8_t> assignment_;
    bool all_stored_finite_=false,finite_fastpath_=false;
};
}
