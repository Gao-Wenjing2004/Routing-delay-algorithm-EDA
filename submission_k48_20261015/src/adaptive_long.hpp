#pragma once
#include "hull_raw.hpp"
#include "projection_core.hpp"
#include "adaptive_d72.hpp"
#include <algorithm>

namespace srb_v11_adaptive_long {
inline constexpr const char* kHullSha="66ae4d1187d05e694f7c3ab82949532daf00e864cfb1010828a3af0fbc1ead51";
inline constexpr const char* kAdaptivePackSha="a141958f3e33c6d089410e97bb77c0eeaaf624312f62e4b7a572bf660776b7a6";
struct Binding {
    std::string hull_asset_sha256,adaptive_pack_sha256;
    std::string deployed_d72_sha256,overlap_proof_sha256,d76_provenance_sha256;
    uint64_t graph_signature;
    uint32_t legacy_hull_bias_crc,expanded_phi_crc,assignment_crc;
};

// Owns ONE replacement Hull loader and borrows the already loaded ShortAdaptive.
// Caller must keep that ShortAdaptive alive. Do not instantiate a second oldHull
// beside this table in production: this table exposes legacy behavior itself.
class Table {
public:
    using Hull=srb_v11_adaptive_long_hull::Table;
    struct PreparedQuery {
        Hull::PreparedQuery old_query{},chosen_query{};
        int16_t phi=0;
        uint8_t medoid=0,quadrant=0,phase=0;
        bool adaptive_eligible=false;
    };
    struct Trace {
        bool old_available=false,chosen_available=false;
        uint32_t old_arc=0;
        int32_t chosen_raw=0;
        Outcome outcome=Outcome::OldUnavailable;
    };
    Table(const std::string& hull_path,const std::string& adaptive_pack_path,
          const srb_v11_adaptive_d72::Table& short_adaptive,const Binding& expected,
          const std::string& deployed_cost_path)
        :hull_(hull_path,expected.graph_signature),adaptive_(short_adaptive){
        const auto& h=hull_.header();const auto& s=adaptive_.legacy();
        if(expected.hull_asset_sha256!=kHullSha||expected.adaptive_pack_sha256!=kAdaptivePackSha
          ||expected.graph_signature!=srb_v11_adaptive_d72::kGraphSignature
          ||h.signature!=expected.graph_signature||h.sources!=110||h.targets!=256||h.prototypes!=32
          ||s.header().signature!=expected.graph_signature||s.header().geometry!=72
          ||s.header().radius!=72||s.header().search!=80||s.header().cells!=21025||s.stored_cells()!=8112
          ||expected.deployed_d72_sha256!="fc77e8416fa33df6644356ee343b7ca99f289847c68d951813b1bbea394cada1"
          ||expected.overlap_proof_sha256!="0825713b90133decd2f18e786101b2888313319afd7c33480bcc1843c157bd3d"
          ||expected.d76_provenance_sha256!="48950b3b96a05a6ba5c4e19693c821e7290e1475bd3abc05ac16dca328427a72")
            fail("Long explicit D72 deployment/D76 parameter provenance binding");
        if(srb_v11_adaptive_a::sha256_file(deployed_cost_path)!=expected.deployed_d72_sha256)
            fail("Long actual deployed D72 cost SHA binding");
        if(srb_v11_adaptive_a::sha256_file(hull_path)!=Hull::deployed_asset_sha256()
          ||srb_v11_adaptive_a::sha256_file(adaptive_pack_path)!=expected.adaptive_pack_sha256)
            fail("Long actual Hull/Adaptive pack SHA binding");
        if(hull_.sources()!=s.sources()||hull_.targets()!=s.targets()||hull_.medoids()!=s.medoids()
          ||hull_.assignment()!=s.assignment())fail("Long ordered source/target/medoid/assignment binding");
        const auto& old=hull_.legacy_bias();const auto& phi=adaptive_.adaptive_bias();const auto& a=adaptive_.adaptive_assignment();
        if(old.size()!=2*12*256||phi.size()!=32*4*120*256||a.size()!=110*4*256
          ||crc32(0,reinterpret_cast<const Bytef*>(old.data()),uInt(old.size()*2))!=expected.legacy_hull_bias_crc
          ||expected.expanded_phi_crc!=3486986971u||expected.assignment_crc!=1194033363u
          ||crc32(0,reinterpret_cast<const Bytef*>(phi.data()),uInt(phi.size()*2))!=expected.expanded_phi_crc
          ||crc32(0,a.data(),uInt(a.size()))!=expected.assignment_crc)
            fail("Long original Hull bias/new phi/assignment CRC binding");
        // Stored Short finite masks provide NO permission to skip Hull checks.
    }
    bool prepare_adaptive(unsigned source,unsigned target,int dx,int dy,
                          bool guarded_original_long,PreparedQuery& p)const{
        const int s=hull_.source_row(source),t=hull_.target_row(target);
        if(s<0||t<0)return false;
        const unsigned old_m=hull_.assignment()[unsigned(t)];
        // This guarded explicit-medoid prepare also checks dy before abs(INT_MIN).
        if(!hull_.prepare_medoid(source,target,dx,dy,old_m,p.old_query))return false;
        p.chosen_query=p.old_query;p.phi=0;p.medoid=uint8_t(old_m);
        p.quadrant=uint8_t(sign_quadrant(dx,dy));p.phase=uint8_t(signed_phase(dx,dy));
        p.adaptive_eligible=guarded_original_long;
        if(!guarded_original_long)return true;
        const unsigned m=adaptive_.adaptive_assignment()[(unsigned(s)*4+p.quadrant)*256+unsigned(t)];
        if(!hull_.prepare_medoid(source,target,dx,dy,m,p.chosen_query))fail("Long chosen prepare inconsistent");
        p.medoid=uint8_t(m);
        p.phi=adaptive_.adaptive_bias()[((m*4+p.quadrant)*120+p.phase)*256+unsigned(t)];
        return true;
    }
    bool query_prepared(const PreparedQuery& p,uint32_t& out,Trace* trace=nullptr)const{
        uint32_t old_arc=0;const bool old_available=hull_.query_prepared(p.old_query,old_arc);
        int32_t chosen_raw=0;bool chosen_available=false;
        if(old_available&&p.adaptive_eligible)chosen_available=hull_.query_medoid_prepared(p.chosen_query,chosen_raw);
        const auto result=project(old_available,old_arc,p.adaptive_eligible,chosen_available,chosen_raw,p.phi);
        if(trace)*trace=Trace{old_available,chosen_available,old_arc,chosen_raw,result.outcome};
        if(!result.available)return false;out=result.arc;return true;
    }
    bool query(unsigned source,unsigned target,int dx,int dy,bool original_long,uint32_t& out,Trace* trace=nullptr)const{
        PreparedQuery p{};return prepare_adaptive(source,target,dx,dy,original_long,p)&&query_prepared(p,out,trace);
    }
    void prefetch(const PreparedQuery& p)const{
        hull_.prefetch(p.old_query);if(p.adaptive_eligible&&p.chosen_query.index!=p.old_query.index)hull_.prefetch(p.chosen_query);
    }
    const Hull& legacy()const{return hull_;}
    // Does not count the borrowed ShortAdaptive a second time.
    size_t memory_bytes()const{return hull_.memory_bytes();}
private:
    [[noreturn]]static void fail(const char* m){throw std::runtime_error(m);}
    Hull hull_;const srb_v11_adaptive_d72::Table& adaptive_;
};
}
