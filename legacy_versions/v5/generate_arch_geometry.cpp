#include "local_arch_data.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
constexpr uint32_t INF = std::numeric_limits<uint32_t>::max();
constexpr int MAX_X = 119, MAX_Y = 549, MARGIN = 16;
constexpr int RX = MAX_X + MARGIN, RY = MAX_Y + MARGIN;
constexpr int W = RX * 2 + 1, H = RY * 2 + 1;
constexpr int OW = MAX_X * 2 + 1, OH = MAX_Y * 2 + 1;
constexpr int ROUTES = srb_local_arch::kRouteCount;

#pragma pack(push, 1)
struct Header { char magic[8]; uint32_t version, max_x, max_y, width, height; };
#pragma pack(pop)

class RadixHeap {
public:
    using Item = std::pair<uint32_t,uint32_t>;
    void push(uint32_t key, uint32_t state) { buckets_[index(key,last_)].push_back({key,state}); ++size_; }
    bool empty() const { return size_ == 0; }
    Item pop() {
        if (buckets_[0].empty()) pull();
        Item v=buckets_[0].back();
        buckets_[0].pop_back();
        --size_;
        return v;
    }
private:
    static int index(uint32_t a,uint32_t b){uint32_t v=a^b;if(!v)return 0;
#if defined(__GNUC__)
        return 32-__builtin_clz(v);
#else
        int n=0;while(v){++n;v>>=1;}return n;
#endif
    }
    void pull(){int i=1;while(i<33&&buckets_[i].empty())++i;uint32_t n=INF;
        for(auto v:buckets_[i]) n=std::min(n,v.first);
        last_=n;std::vector<Item> tmp;tmp.swap(buckets_[i]);
        for(auto v:tmp)buckets_[index(v.first,last_)].push_back(v);}
    std::array<std::vector<Item>,33> buckets_{}; uint32_t last_=0; uint64_t size_=0;
};

inline uint32_t sid(int x,int y,int r){return static_cast<uint32_t>((static_cast<uint64_t>(y)*W+x)*ROUTES+r);}
}

int main(int argc,char** argv){
    try {
        if(argc!=2){std::cerr<<"Usage: "<<argv[0]<<" arch_geometry.bin\n";return 2;}
        const size_t states=static_cast<size_t>(W)*H*ROUTES;
        std::vector<uint32_t> dist(states,INF);
        std::vector<uint16_t> result(static_cast<size_t>(OW)*OH,UINT16_MAX);
        RadixHeap heap;
        for(int r=0;r<ROUTES;++r){uint32_t s=sid(RX,RY,r);dist[s]=0;heap.push(0,s);}
        uint64_t remaining=static_cast<uint64_t>(OW)*OH,expanded=0;
        auto begin=std::chrono::steady_clock::now();
        while(!heap.empty()&&remaining){auto item=heap.pop();uint32_t d=item.first,s=item.second;if(dist[s]!=d)continue;
            int r=s%ROUTES;uint32_t cell=s/ROUTES;int x=cell%W,y=cell/W;++expanded;
            int dx=x-RX,dy=y-RY;
            if(std::abs(dx)<=MAX_X&&std::abs(dy)<=MAX_Y){size_t oi=static_cast<size_t>(dy+MAX_Y)*OW+dx+MAX_X;
                if(result[oi]==UINT16_MAX){if(d>=UINT16_MAX)throw std::runtime_error("geometry delay exceeds uint16");result[oi]=static_cast<uint16_t>(d);--remaining;}}
            uint16_t input=srb_local_arch::kRouteToInput[r];
            for(uint32_t ei=srb_local_arch::kTransitionOffset[input];ei<srb_local_arch::kTransitionOffset[input+1];++ei){
                const auto&e=srb_local_arch::kTransitions[ei];int nx=x+e.dx,ny=y+e.dy;if(nx<0||nx>=W||ny<0||ny>=H)continue;
                uint32_t ns=sid(nx,ny,e.next_route),nd=d+e.cost;if(nd<dist[ns]){dist[ns]=nd;heap.push(nd,ns);}}
        }
        if(remaining)throw std::runtime_error("geometry search box was insufficient");
        std::ofstream out(argv[1],std::ios::binary);if(!out)throw std::runtime_error("cannot create output");
        Header h{};std::memcpy(h.magic,"SRBGEO1",7);h.version=1;h.max_x=MAX_X;h.max_y=MAX_Y;h.width=OW;h.height=OH;
        out.write(reinterpret_cast<const char*>(&h),sizeof(h));out.write(reinterpret_cast<const char*>(result.data()),result.size()*2);out.flush();
        double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
        std::cerr<<"generated="<<argv[1]<<" values="<<result.size()<<" expanded="<<expanded
                 <<" elapsed="<<std::fixed<<std::setprecision(3)<<sec<<" s\n"
                 <<"data_source=local_arch_data.hpp (JSON only; no Golden CSV)\n";
        return 0;
    }catch(const std::exception&e){std::cerr<<"error: "<<e.what()<<"\n";return 1;}
}
