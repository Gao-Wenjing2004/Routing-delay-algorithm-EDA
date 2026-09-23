#pragma once

#ifndef SRB_MODEL_HEADER
#define SRB_MODEL_HEADER "fast_model_data.hpp"
#endif
#include SRB_MODEL_HEADER
#ifdef SRB_RESIDUAL_HEADER
#include SRB_RESIDUAL_HEADER
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <array>
#include <map>
#include <vector>

namespace srb_fast {

struct Slice {
    const char* data;
    size_t size;

    Slice() : data(nullptr), size(0) {}
    Slice(const char* input_data, size_t input_size) : data(input_data), size(input_size) {}
};

struct Endpoint {
    int16_t x = 0;
    int16_t y = 0;
    uint16_t port = 0;
};

class FastEstimator {
public:
    FastEstimator() {
        for (int i = 0; i < 550; ++i) sqrt_distance_[i] = std::sqrt(static_cast<float>(i));
        std::fill(port_slots_, port_slots_ + 2048, -1);
        for (int i = 0; i < srb_fast_data::kPortCount; ++i) {
            const char* name = srb_fast_data::kSortedPortNames[i];
            port_lengths_[i] = static_cast<uint16_t>(std::strlen(name));
            uint32_t slot = hash_port(Slice{name, port_lengths_[i]}) & 2047;
            while (port_slots_[slot] >= 0) slot = (slot + 1) & 2047;
            port_slots_[slot] = static_cast<int16_t>(i);
        }
#ifdef SRB_MERGED_MODEL
        prepare_merged_tables();
#endif
    }

    bool parse_endpoint(Slice text, Endpoint& out) const {
        trim(text);
        if (text.size < 9 || std::memcmp(text.data, "SRB_", 4) != 0) return false;

        size_t pos = 4;
        int x = 0, y = 0;
        if (!parse_nonnegative(text, pos, x)) return false;
        if (pos >= text.size || text.data[pos++] != '_') return false;
        if (!parse_nonnegative(text, pos, y)) return false;
        if (pos >= text.size || text.data[pos++] != '/') return false;
        if (x < 0 || x >= 120 || y < 0 || y >= 550 || pos >= text.size) return false;

        Slice port{text.data + pos, text.size - pos};
        int pid = find_port(port);
        if (pid < 0) return false;
        out.x = static_cast<int16_t>(x);
        out.y = static_cast<int16_t>(y);
        out.port = static_cast<uint16_t>(pid);
        return true;
    }

    bool predict_spec(Slice from, Slice to, uint32_t& result) const {
        Endpoint a, b;
        if (!parse_endpoint(from, a) || !parse_endpoint(to, b)) return false;
        result = predict(a, b);
        return true;
    }

#ifdef SRB_MERGED_MODEL
    bool local_lookup_allowed(const Endpoint&a,const Endpoint&b)const {
        return local_safe_[block_class_[a.x*550+a.y]*block_classes_+block_class_[b.x*550+b.y]]!=0;
    }
#endif

    uint32_t predict(const Endpoint& a, const Endpoint& b) const {
        using namespace srb_fast_data;
        if (a.x == b.x && a.y == b.y && a.port == b.port) return 0;
#ifdef SRB_MERGED_MODEL
        const auto& g=geometry_[(int(b.x)-a.x+119)*1099+int(b.y)-a.y+549];
        float value=g.value+source_distance_[a.port*144+g.direction_distance]
                         +target_distance_[b.port*144+g.direction_distance];
        value+=kPortPair[static_cast<size_t>(a.port)*kPortCount+b.port];
        value+=kSrcRegion[(a.x/8)*69+a.y/8];
        value+=kDstRegion[(b.x/8)*69+b.y/8];
        value+=kSrcRemainder[a.port*289+g.remainder];
        value+=kDstRemainder[b.port*289+g.remainder];
#ifdef SRB_RESIDUAL_HEADER
        if(g.correction!=65535){
#ifdef SRB_RESIDUAL_COMPACT
            const unsigned shape=g.correction/15;
            const unsigned xc=(g.correction%15)*3+g.direction_distance/48;
            value+=srb_residual::source_shape[a.port*72+shape];
            value+=srb_residual::target_shape[b.port*72+shape];
            value+=srb_residual::source_x[a.port*45+xc];
            value+=srb_residual::target_x[b.port*45+xc];
#else
            value+=srb_residual::source[a.port*1080+g.correction];
            value+=srb_residual::target[b.port*1080+g.correction];
#endif
        }
#endif
        value+=block_effect_[block_class_[a.x*550+a.y]*block_classes_+block_class_[b.x*550+b.y]];
        value+=std::abs(int(kXGapPrefix[b.x])-kXGapPrefix[a.x]);
        value+=std::abs(int(kYGapPrefix[b.y])-kYGapPrefix[a.y]);
        return value>0?static_cast<uint32_t>(std::floor(double(value)+0.5)):0;
#else

        const int dx = static_cast<int>(b.x) - a.x;
        const int dy = static_cast<int>(b.y) - a.y;
        const int ax = std::abs(dx);
        const int ay = std::abs(dy);
        const int xp = std::max(dx, 0);
        const int xn = std::max(-dx, 0);
        const int yp = std::max(dy, 0);
        const int yn = std::max(-dy, 0);

        float value = kBeta[0]
                    + kBeta[1] * xp
                    + kBeta[2] * xn
                    + kBeta[3] * yp
                    + kBeta[4] * yn
                    + kBeta[5] * sqrt_distance_[xp]
                    + kBeta[6] * sqrt_distance_[xn]
                    + kBeta[7] * sqrt_distance_[yp]
                    + kBeta[8] * sqrt_distance_[yn]
                    + kBeta[9] * (dx == 0 ? 1.0f : 0.0f)
                    + kBeta[10] * (dy == 0 ? 1.0f : 0.0f);

        const int quad = (dx > 0 ? 1 : 0)
                       + (dx < 0 ? 2 : 0)
                       + (dy > 0 ? 3 : 0)
                       + (dy < 0 ? 6 : 0);
        value += kSrcQuad[static_cast<size_t>(a.port) * 9 + quad];
        value += kDstQuad[static_cast<size_t>(b.port) * 9 + quad];

        const int rx = (dx > 0 ? ax % 8 : (dx < 0 ? -(ax % 8) : 0)) + 8;
        const int ry = (dy > 0 ? ay % 8 : (dy < 0 ? -(ay % 8) : 0)) + 8;
        value += kRemainder[rx * 17 + ry];

        const int distance_bin = std::min(15, std::max(ax, ay) / 32);
        value += kDistanceBin[quad * 16 + distance_bin];

        const int gx = std::min(14, ax / 8);
        const int gy = std::min(68, ay / 8);
        value += kGeom8[quad * (15 * 69) + gx * 69 + gy];

        const int ratio = std::min(16, 17 * std::min(ax, ay) / (std::max(ax, ay) + 1));
        value += kAngle[quad * 17 + ratio];
        value += kPortPair[static_cast<size_t>(a.port) * kPortCount + b.port];
        value += kSrcDistance[(static_cast<size_t>(a.port) * 9 + quad) * 16 + distance_bin];
        value += kDstDistance[(static_cast<size_t>(b.port) * 9 + quad) * 16 + distance_bin];
        value += kSrcRegion[(a.x / 8) * 69 + a.y / 8];
        value += kDstRegion[(b.x / 8) * 69 + b.y / 8];

        const int remainder = rx * 17 + ry;
        value += kSrcRemainder[static_cast<size_t>(a.port) * (17 * 17) + remainder];
        value += kDstRemainder[static_cast<size_t>(b.port) * (17 * 17) + remainder];
        value += kGeom4[quad * (30 * 138) + std::min(29, ax / 4) * 138 + std::min(137, ay / 4)];
        value += kDisplacement[static_cast<size_t>(dx + 119) * 1099 + (dy + 549)];

        for (int bi = 0; bi < kBlockCount; ++bi) {
            const int code = side(a.x, kBlockLeft[bi], kBlockRight[bi])
                           + 3 * side(b.x, kBlockLeft[bi], kBlockRight[bi])
                           + 9 * side(a.y, kBlockLower[bi], kBlockUpper[bi])
                           + 27 * side(b.y, kBlockLower[bi], kBlockUpper[bi]);
            value += kBlockEffect[bi * 81 + code];
        }

#ifdef SRB_SPATIAL_MODEL
        value += extra_src_xquad[a.x * 9 + quad];
        value += extra_dst_xquad[b.x * 9 + quad];
        value += extra_xpair[(a.x * 120 + b.x) * 3 + (dy > 0 ? 1 : dy < 0 ? 2 : 0)];
        value += extra_ypair[(a.y / 8) * 69 + b.y / 8];
        for (int bi = 0; bi < kBlockCount; ++bi) {
            int code = side(a.x, kBlockLeft[bi], kBlockRight[bi]) + 3 * side(b.x, kBlockLeft[bi], kBlockRight[bi])
                     + 9 * side(a.y, kBlockLower[bi], kBlockUpper[bi]) + 27 * side(b.y, kBlockLower[bi], kBlockUpper[bi]);
            value += extra_blockquad[bi * 81 * 9 + code * 9 + quad];
        }
#endif

        value += std::abs(static_cast<int>(kXGapPrefix[b.x]) - kXGapPrefix[a.x]);
        value += std::abs(static_cast<int>(kYGapPrefix[b.y]) - kYGapPrefix[a.y]);

        if (!(value > 0.0f)) return 0;
        const double rounded = std::floor(static_cast<double>(value) + 0.5);
        if (rounded >= static_cast<double>(std::numeric_limits<uint32_t>::max())) {
            return std::numeric_limits<uint32_t>::max();
        }
        return static_cast<uint32_t>(rounded);
#endif
    }

private:
#ifdef SRB_MERGED_MODEL
    struct Geometry { float value; uint16_t direction_distance, remainder;
#ifdef SRB_RESIDUAL_HEADER
        uint16_t correction;
#endif
    };
    std::vector<Geometry> geometry_;
    std::vector<float> source_distance_,target_distance_,block_effect_;
    std::vector<uint16_t> block_class_;
    std::vector<uint8_t> local_safe_;
    unsigned block_classes_=0;
    void prepare_merged_tables(){
        using namespace srb_fast_data;
        geometry_.resize(239*1099);source_distance_.resize(kPortCount*144);target_distance_.resize(kPortCount*144);
        for(int dx=-119;dx<=119;++dx)for(int dy=-549;dy<=549;++dy){
            int ax=std::abs(dx),ay=std::abs(dy),xp=std::max(dx,0),xn=std::max(-dx,0),yp=std::max(dy,0),yn=std::max(-dy,0);
            int q=(dx>0)+2*(dx<0)+3*(dy>0)+6*(dy<0),bin=std::min(15,std::max(ax,ay)/32);
            int rx=(dx>0?ax%8:dx<0?-(ax%8):0)+8,ry=(dy>0?ay%8:dy<0?-(ay%8):0)+8,rem=rx*17+ry;
            float v=kBeta[0]+kBeta[1]*xp+kBeta[2]*xn+kBeta[3]*yp+kBeta[4]*yn+kBeta[5]*sqrt_distance_[xp]+kBeta[6]*sqrt_distance_[xn]+kBeta[7]*sqrt_distance_[yp]+kBeta[8]*sqrt_distance_[yn]+kBeta[9]*(dx==0)+kBeta[10]*(dy==0);
            v+=kRemainder[rem];v+=kDistanceBin[q*16+bin];v+=kGeom8[q*(15*69)+std::min(14,ax/8)*69+std::min(68,ay/8)];
            v+=kAngle[q*17+std::min(16,17*std::min(ax,ay)/(std::max(ax,ay)+1))];
            v+=kGeom4[q*(30*138)+std::min(29,ax/4)*138+std::min(137,ay/4)];v+=kDisplacement[(dx+119)*1099+dy+549];
            geometry_[(dx+119)*1099+dy+549]={v,static_cast<uint16_t>(q*16+bin),static_cast<uint16_t>(rem)};
#ifdef SRB_RESIDUAL_HEADER
            geometry_[(dx+119)*1099+dy+549].correction=std::max(ax,ay)>72?static_cast<uint16_t>((q*8+std::min(7,8*ax/(ax+ay+1)))*15+std::min(14,(dx+119)/16)):65535;
#endif
        }
        for(int p=0;p<kPortCount;++p)for(int q=0;q<9;++q)for(int b=0;b<16;++b){
            unsigned i=(p*9+q)*16+b;source_distance_[i]=kSrcQuad[p*9+q]+kSrcDistance[i];target_distance_[i]=kDstQuad[p*9+q]+kDstDistance[i];
        }
        using Signature=std::array<uint8_t,kBlockCount*2>;std::map<Signature,uint16_t> classes;std::vector<Signature> signatures;
        block_class_.resize(120*550);
        for(int x=0;x<120;++x)for(int y=0;y<550;++y){Signature s{};for(int i=0;i<kBlockCount;++i){s[i*2]=side(x,kBlockLeft[i],kBlockRight[i]);s[i*2+1]=side(y,kBlockLower[i],kBlockUpper[i]);}
            auto [it,inserted]=classes.emplace(s,classes.size());if(inserted)signatures.push_back(s);block_class_[x*550+y]=it->second;
        }
        block_classes_=classes.size();block_effect_.resize(block_classes_*block_classes_);local_safe_.resize(block_classes_*block_classes_,1);
        for(unsigned a=0;a<block_classes_;++a)for(unsigned b=0;b<block_classes_;++b){float v=0;for(int i=0;i<kBlockCount;++i){auto&s=signatures[a];auto&t=signatures[b];int sx=s[i*2],tx=t[i*2],sy=s[i*2+1],ty=t[i*2+1];int code=sx+3*tx+9*sy+27*ty;v+=kBlockEffect[i*81+code];
                bool crossx=std::abs(sx-tx)==2,crossy=std::abs(sy-ty)==2;
                bool sameoutsidey=sy==ty&&sy!=1,sameoutsidex=sx==tx&&sx!=1;
                if((crossx&&!sameoutsidey)||(crossy&&!sameoutsidex))local_safe_[a*block_classes_+b]=0;
            }block_effect_[a*block_classes_+b]=v;}
    }
#endif
    float sqrt_distance_[550] = {};
    int16_t port_slots_[2048];
    uint16_t port_lengths_[srb_fast_data::kPortCount];

    static uint32_t hash_port(Slice name) {
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < name.size; ++i) h = (h ^ static_cast<unsigned char>(name.data[i])) * 16777619u;
        return h;
    }

    static void trim(Slice& value) {
        while (value.size && (value.data[0] == ' ' || value.data[0] == '\t' || value.data[0] == '\r')) {
            ++value.data;
            --value.size;
        }
        while (value.size) {
            char c = value.data[value.size - 1];
            if (c != ' ' && c != '\t' && c != '\r') break;
            --value.size;
        }
    }

    static bool parse_nonnegative(Slice text, size_t& pos, int& value) {
        if (pos >= text.size || text.data[pos] < '0' || text.data[pos] > '9') return false;
        int result = 0;
        while (pos < text.size && text.data[pos] >= '0' && text.data[pos] <= '9') {
            if (result > 100000) return false;
            result = result * 10 + (text.data[pos] - '0');
            ++pos;
        }
        value = result;
        return true;
    }

    static int compare(Slice a, const char* b) {
        const size_t bn = std::strlen(b);
        const size_t n = std::min(a.size, bn);
        int c = std::memcmp(a.data, b, n);
        if (c != 0) return c;
        if (a.size < bn) return -1;
        if (a.size > bn) return 1;
        return 0;
    }

    int find_port(Slice name) const {
        uint32_t slot = hash_port(name) & 2047;
        for (unsigned probe = 0; probe < 2048; ++probe, slot = (slot + 1) & 2047) {
            int index = port_slots_[slot];
            if (index < 0) return -1;
            if (name.size == port_lengths_[index] && std::memcmp(name.data, srb_fast_data::kSortedPortNames[index], name.size) == 0)
                return srb_fast_data::kSortedPortIds[index];
        }
        return -1;
    }

    static int side(int value, int low, int high) {
        if (value < low) return 0;
        if (value > high) return 2;
        return 1;
    }
};

}  // namespace srb_fast
