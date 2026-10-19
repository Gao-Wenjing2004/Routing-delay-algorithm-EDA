#pragma once
#include "srb_fast.hpp"
#ifndef SRB_NORM_ARCH_HEADER
#define SRB_NORM_ARCH_HEADER "norm_arch_data.hpp"
#endif
#include SRB_NORM_ARCH_HEADER
#ifdef SRB_NORM_PHASE_HEADER
#include SRB_NORM_PHASE_HEADER
#endif
#ifndef SRB_NORMALIZE_OUTPUTS
#define SRB_NORMALIZE_OUTPUTS 1
#endif

namespace srb_normalization {
using srb_fast::Endpoint;
inline constexpr uint32_t kUnreachableDelay = std::numeric_limits<uint32_t>::max();

class ForcedNetNormalizer {
public:
    ForcedNetNormalizer() : moves_(srb_norm_arch::kGroupCount*66000,0xffffffffu), exceptional_((srb_norm_arch::kGroupCount*66000+7)/8,0) {
        using namespace srb_norm_arch;
        for(int g=0;g<kGroupCount;++g)for(int sx=0;sx<120;++sx)for(int sy=0;sy<550;++sy){
            const int source=sx*550+sy;
            if(!real(source))continue;
            const int dx=offsets[g*2],dy=offsets[g*2+1];
            const bool h=dx!=0;
            const int step=(h?dx:dy)>0?1:-1,need=std::abs(h?dx:dy);
            int x=sx,y=sy,count=0;
            unsigned charged=0,extra=0;
            bool valid=true;
            while(count<need){
                if(h)x+=step;else y+=step;
                if(x<0||x>=120||y<0||y>=550){valid=false;break;}
                if(real(x*550+y)){++count;continue;}
                int bi=-1;
                for(unsigned k=0;k<sizeof(blocks)/sizeof(blocks[0]);++k){
                    const auto&b=blocks[k];
                    if(x>=b.left&&x<=b.right&&y>=b.lower&&y<=b.upper)bi=k;
                }
                if(bi>=0&&(h?blocks[bi].hc:blocks[bi].vc)){
                    if(!(charged&(1u<<bi))){charged|=1u<<bi;extra+=h?blocks[bi].hcost:blocks[bi].vcost;}
                }else ++count;
            }
            if(!valid||!real(x*550+y))continue;
            extra+=std::abs(int(srb_fast_data::kXGapPrefix[x])-srb_fast_data::kXGapPrefix[sx]);
            extra+=std::abs(int(srb_fast_data::kYGapPrefix[y])-srb_fast_data::kYGapPrefix[sy]);
            moves_[g*66000+source]=unsigned((x<<10)|y)|(extra<<17);
        }
        // A 132 KB bitmap retains exact behavior for every source/group while
        // keeping ordinary translation off the 4.224 MB random-move table.
        for(int g=0;g<kGroupCount;++g)for(int x=0;x<120;++x)for(int y=0;y<550;++y){
            const unsigned index=g*66000+x*550+y;
            const int nx=x+offsets[g*2],ny=y+offsets[g*2+1];
            bool exceptional=nx<0||nx>=120||ny<0||ny>=550;
            if(!exceptional){
                const unsigned extra=std::abs(int(srb_fast_data::kXGapPrefix[nx])-srb_fast_data::kXGapPrefix[x])
                                    +std::abs(int(srb_fast_data::kYGapPrefix[ny])-srb_fast_data::kYGapPrefix[y]);
                exceptional=moves_[index]!=(unsigned((nx<<10)|ny)|(extra<<17));
            }
            if(exceptional)exceptional_[index>>3]|=uint8_t(1u<<(index&7));
        }
    }

    // Input sources need no forced Net. False proves that an Output source has
    // no outgoing physical Net; this guard does not claim general reachability.
    bool has_forced_net(const Endpoint& source)const{
        const int g=srb_norm_arch::net_group[source.port];
        if(g<0)return !srb_norm_arch::is_output[source.port];
        const unsigned index=g*66000+source.x*550+source.y;
        return !(exceptional_[index>>3]&(1u<<(index&7)))||moves_[index]!=0xffffffffu;
    }

    // Returns false only when the forced source Net does not exist physically.
    bool normalize(const Endpoint& source,Endpoint& result,uint32_t& extra)const{
        result=source;extra=0;
        const int g=srb_norm_arch::net_group[source.port];
        if(g<0)return !srb_norm_arch::is_output[source.port];
        const unsigned index=g*66000+source.x*550+source.y;
        if(!(exceptional_[index>>3]&(1u<<(index&7)))){
            result.x=source.x+srb_norm_arch::offsets[g*2];
            result.y=source.y+srb_norm_arch::offsets[g*2+1];
            result.port=srb_norm_arch::net_target[source.port];
            extra=std::abs(int(srb_fast_data::kXGapPrefix[result.x])-srb_fast_data::kXGapPrefix[source.x])
                 +std::abs(int(srb_fast_data::kYGapPrefix[result.y])-srb_fast_data::kYGapPrefix[source.y]);
            return true;
        }
        const uint32_t packed=moves_[index];
        if(packed==0xffffffffu)return false;
        const unsigned pos=packed&131071u;
        result.x=pos>>10;result.y=pos&1023;result.port=srb_norm_arch::net_target[source.port];
        extra=packed>>17;
        return true;
    }

    static int direct_arc(unsigned from,unsigned to){
        using namespace srb_norm_arch;
        unsigned lo=direct_offset[from],hi=direct_offset[from+1];
        while(lo<hi){const unsigned mid=(lo+hi)/2;if(direct[mid].port<to)lo=mid+1;else hi=mid;}
        return lo<direct_offset[from+1]&&direct[lo].port==to?direct[lo].cost:-1;
    }
    const std::vector<uint32_t>& moves()const{return moves_;}
private:
    static bool real(unsigned p){return (srb_norm_arch::real_bits[p>>3]>>(p&7))&1;}
    std::vector<uint32_t> moves_;
    std::vector<uint8_t> exceptional_;
};

class NormalizedEstimator{
public:
    bool local_lookup_allowed(const Endpoint&a,const Endpoint&b)const{return model_.local_lookup_allowed(a,b);}
    bool has_forced_net(const Endpoint& source)const{return normalizer_.has_forced_net(source);}
    bool parse_endpoint(srb_fast::Slice s,Endpoint&e)const{return model_.parse_endpoint(s,e);}
    bool predict_spec(srb_fast::Slice from,srb_fast::Slice to,uint32_t&v)const{
        Endpoint a,b;if(!parse_endpoint(from,a)||!parse_endpoint(to,b))return false;v=predict(a,b);return true;
    }
    unsigned native_correction_code(const Endpoint&a,const Endpoint&b)const{return model_.native_correction_code(a,b);}
    bool normalize_source(const Endpoint&a,Endpoint&n,uint32_t&extra)const{return normalizer_.normalize(a,n,extra);}
    uint32_t predict(const Endpoint&a,const Endpoint&b)const{
        if(a.x==b.x&&a.y==b.y&&a.port==b.port)return 0;
        Endpoint n=a;uint32_t extra=0;
#if SRB_NORMALIZE_OUTPUTS
        if(!normalizer_.normalize(a,n,extra))return kUnreachableDelay;
#endif
        return predict_normalized(a,b,n,extra);
    }
    uint32_t predict_normalized(const Endpoint&a,const Endpoint&b,const Endpoint&n,uint32_t extra)const{
        if(n.x==b.x&&n.y==b.y){
            if(n.port==b.port)return extra;
            // For this architecture, max direct Arc=135 < 3*min Arc=144.
            // Any alternate return to this tile plus terminal Arc needs >=3 Arcs.
            int direct=ForcedNetNormalizer::direct_arc(n.port,b.port);
            if(direct>=0)return extra+direct;
        }
        const uint32_t base=extra+model_.predict(n,b);
#ifdef SRB_NORM_PHASE_HEADER
        const int original_cheb=std::max(std::abs(int(b.x)-a.x),std::abs(int(b.y)-a.y));
        float phase_correction=0;
        if(srb_norm_phase::kScope!=0&&!(srb_norm_phase::kScope==2&&original_cheb<=72)){
            const unsigned phase=model_.phase_code(n,b);
            phase_correction=srb_norm_phase::source[n.port*120+phase]+srb_norm_phase::target[b.port*120+phase];
        }
        const double corrected=double(base)+phase_correction;
        return corrected>0?uint32_t(corrected+0.5):0;
#else
        return base;
#endif
    }
private:
    srb_fast::FastEstimator model_;
    ForcedNetNormalizer normalizer_;
};
}
