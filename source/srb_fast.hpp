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
#ifdef SRB_ENDPOINT_META_CACHE
    uint16_t block_class = 0;
    uint16_t region = 0;
#endif
};

class FastEstimator {
public:
    FastEstimator() {
        for (int i = 0; i < 550; ++i) sqrt_distance_[i] = std::sqrt(static_cast<float>(i));
#ifndef SRB_FAST_PORT_DECODE
        std::fill(port_slots_, port_slots_ + 2048, -1);
        for (int i = 0; i < srb_fast_data::kPortCount; ++i) {
            const char* name = srb_fast_data::kSortedPortNames[i];
            port_lengths_[i] = static_cast<uint16_t>(std::strlen(name));
            uint32_t slot = hash_port(Slice{name, port_lengths_[i]}) & 2047;
            while (port_slots_[slot] >= 0) slot = (slot + 1) & 2047;
            port_slots_[slot] = static_cast<int16_t>(i);
        }
#endif
#ifdef SRB_MERGED_MODEL
        prepare_merged_tables();
#endif
    }

    bool parse_endpoint(Slice text, Endpoint& out) const {
        trim(text);
        return parse_endpoint_pretrimmed(text, out);
    }

    bool parse_endpoint_pretrimmed(Slice text, Endpoint& out) const {
        if (text.size < 9 || std::memcmp(text.data, "SRB_", 4) != 0) return false;

        size_t pos = 4;
        int x = 0, y = 0;
#ifdef SRB_FAST_COORD_DECODE
        if (!parse_grid_coordinate(text, pos, '_', 120, x) ||
            !parse_grid_coordinate(text, pos, '/', 550, y)) return false;
#else
        if (!parse_nonnegative(text, pos, x)) return false;
        if (pos >= text.size || text.data[pos++] != '_') return false;
        if (!parse_nonnegative(text, pos, y)) return false;
        if (pos >= text.size || text.data[pos++] != '/') return false;
        if (x < 0 || x >= 120 || y < 0 || y >= 550) return false;
#endif
        if (pos >= text.size) return false;

        Slice port{text.data + pos, text.size - pos};
        int pid =
#ifdef SRB_FAST_PORT_DECODE
            find_port_structured(port);
#else
            find_port(port);
#endif
        if (pid < 0) return false;
        out.x = static_cast<int16_t>(x);
        out.y = static_cast<int16_t>(y);
        out.port = static_cast<uint16_t>(pid);
#ifdef SRB_ENDPOINT_META_CACHE
        out.block_class = block_class_[x * 550 + y];
        out.region = static_cast<uint16_t>((x / 8) * 69 + y / 8);
#endif
        return true;
    }

    bool predict_spec(Slice from, Slice to, uint32_t& result) const {
        Endpoint a, b;
        if (!parse_endpoint(from, a) || !parse_endpoint(to, b)) return false;
        result = predict(a, b);
        return true;
    }

#if defined(SRB_ENDPOINT_META_CACHE) && defined(SRB_LANDING_BLOCK_CONTEXT)
    void refresh_endpoint_metadata(Endpoint& endpoint) const {
        endpoint.block_class = block_class_[endpoint.x * 550 + endpoint.y];
        endpoint.region = static_cast<uint16_t>((endpoint.x / 8) * 69 + endpoint.y / 8);
    }
#endif

#ifdef SRB_MERGED_MODEL
    bool local_lookup_allowed(const Endpoint&a,const Endpoint&b)const {
#ifdef SRB_ENDPOINT_META_CACHE
        return local_safe_[a.block_class*block_classes_+b.block_class]!=0;
#else
        return local_safe_[block_class_[a.x*550+a.y]*block_classes_+block_class_[b.x*550+b.y]]!=0;
#endif
    }
    uint16_t local_lookup_delay(const Endpoint&a,const Endpoint&b)const {
        using namespace srb_fast_data;
#ifdef SRB_ENDPOINT_META_CACHE
        uint32_t value=local_cross_delay_[a.block_class*block_classes_+b.block_class];
#else
        uint32_t value=local_cross_delay_[block_class_[a.x*550+a.y]*block_classes_+block_class_[b.x*550+b.y]];
#endif
        bool vertical_detour=false;
#ifdef SRB_VERTICAL_DETOUR_CACHE
        vertical_detour=local_vertical_detour_[a.block_class*block_classes_+b.block_class]!=0;
#else
        for(int i=1;i<kBlockCount;++i){
            int sy=side(a.y,kBlockLower[i],kBlockUpper[i]),ty=side(b.y,kBlockLower[i],kBlockUpper[i]);
            int sx=side(a.x,kBlockLeft[i],kBlockRight[i]),tx=side(b.x,kBlockLeft[i],kBlockRight[i]);
            if(std::abs(sy-ty)==2&&!(sx==tx&&sx!=1))vertical_detour=true;
        }
#endif
        if(vertical_detour){
            int left=std::abs(int(a.x)-75)+std::abs(int(b.x)-75);
            int right=std::abs(int(a.x)-90)+std::abs(int(b.x)-90);
#ifdef SRB_DETOUR_GAP_COST
            const int direct=std::abs(int(b.x)-a.x);
            const int direct_gap=std::abs(int(kXGapPrefix[b.x])-kXGapPrefix[a.x]);
            const int left_gap=std::abs(int(kXGapPrefix[a.x])-kXGapPrefix[75])+
                               std::abs(int(kXGapPrefix[b.x])-kXGapPrefix[75])-direct_gap;
            const int right_gap=std::abs(int(kXGapPrefix[a.x])-kXGapPrefix[90])+
                                std::abs(int(kXGapPrefix[b.x])-kXGapPrefix[90])-direct_gap;
            const int left_extra=16*(left-direct)+left_gap;
            const int right_extra=16*(right-direct)+right_gap;
            const int extra=std::min(left_extra,right_extra);
            if(extra>0)value+=extra;
#else
            int extra=std::min(left,right)-std::abs(int(b.x)-a.x);
            if(extra>0)value+=16*extra;
#endif
        }
        return static_cast<uint16_t>(value);
    }
#ifdef SRB_BLOCK_FOLD_CACHE
    int local_dx_fold(const Endpoint&a,const Endpoint&b)const {
        return local_dx_fold_[a.block_class*block_classes_+b.block_class];
    }
#endif
#endif

    uint32_t predict(const Endpoint& a, const Endpoint& b) const {
        using namespace srb_fast_data;
        if (a.x == b.x && a.y == b.y && a.port == b.port) return 0;
#ifdef SRB_MERGED_MODEL
        const auto& g=geometry_[(int(b.x)-a.x+119)*1099+int(b.y)-a.y+549];
        float value=g.value+source_distance_[a.port*144+g.direction_distance]
                         +target_distance_[b.port*144+g.direction_distance];
#ifndef SRB_NO_PORT_PAIR
        value+=kPortPair[static_cast<size_t>(a.port)*kPortCount+b.port];
#endif
#ifndef SRB_NO_REGION
#ifdef SRB_ENDPOINT_META_CACHE
        value+=kSrcRegion[a.region];
        value+=kDstRegion[b.region];
#else
        value+=kSrcRegion[(a.x/8)*69+a.y/8];
        value+=kDstRegion[(b.x/8)*69+b.y/8];
#endif
#endif
#ifndef SRB_NO_PORT_REMAINDER
        value+=kSrcRemainder[a.port*289+g.remainder];
        value+=kDstRemainder[b.port*289+g.remainder];
#endif
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
#ifdef SRB_ENDPOINT_META_CACHE
        value+=block_effect_[a.block_class*block_classes_+b.block_class];
#else
        value+=block_effect_[block_class_[a.x*550+a.y]*block_classes_+block_class_[b.x*550+b.y]];
#endif
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
#ifndef SRB_NO_PORT_PAIR
        value += kPortPair[static_cast<size_t>(a.port) * kPortCount + b.port];
#endif
        value += kSrcDistance[(static_cast<size_t>(a.port) * 9 + quad) * 16 + distance_bin];
        value += kDstDistance[(static_cast<size_t>(b.port) * 9 + quad) * 16 + distance_bin];
#ifndef SRB_NO_REGION
        value += kSrcRegion[(a.x / 8) * 69 + a.y / 8];
        value += kDstRegion[(b.x / 8) * 69 + b.y / 8];
#endif

        const int remainder = rx * 17 + ry;
#ifndef SRB_NO_PORT_REMAINDER
        value += kSrcRemainder[static_cast<size_t>(a.port) * (17 * 17) + remainder];
        value += kDstRemainder[static_cast<size_t>(b.port) * (17 * 17) + remainder];
#endif
        value += kGeom4[quad * (30 * 138) + std::min(29, ax / 4) * 138 + std::min(137, ay / 4)];
#ifndef SRB_NO_DISPLACEMENT
        value += kDisplacement[static_cast<size_t>(dx + 119) * 1099 + (dy + 549)];
#endif

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
    std::vector<uint16_t> local_cross_delay_;
#ifdef SRB_BLOCK_FOLD_CACHE
    std::vector<int16_t> local_dx_fold_;
#endif
#ifdef SRB_VERTICAL_DETOUR_CACHE
    std::vector<uint8_t> local_vertical_detour_;
#endif
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
            v+=kGeom4[q*(30*138)+std::min(29,ax/4)*138+std::min(137,ay/4)];
#ifndef SRB_NO_DISPLACEMENT
            v+=kDisplacement[(dx+119)*1099+dy+549];
#endif
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
            auto inserted_pair=classes.emplace(s,classes.size());if(inserted_pair.second)signatures.push_back(s);block_class_[x*550+y]=inserted_pair.first->second;
        }
        block_classes_=classes.size();block_effect_.resize(block_classes_*block_classes_);local_safe_.resize(block_classes_*block_classes_,1);local_cross_delay_.resize(block_classes_*block_classes_);
#ifdef SRB_BLOCK_FOLD_CACHE
        local_dx_fold_.resize(block_classes_*block_classes_);
#endif
#ifdef SRB_VERTICAL_DETOUR_CACHE
        local_vertical_detour_.resize(block_classes_*block_classes_);
#endif
        for(unsigned a=0;a<block_classes_;++a)for(unsigned b=0;b<block_classes_;++b){float v=0;for(int i=0;i<kBlockCount;++i){auto&s=signatures[a];auto&t=signatures[b];int sx=s[i*2],tx=t[i*2],sy=s[i*2+1],ty=t[i*2+1];int code=sx+3*tx+9*sy+27*ty;v+=kBlockEffect[i*81+code];
                bool crossx=std::abs(sx-tx)==2,crossy=std::abs(sy-ty)==2;
                bool sameoutsidey=sy==ty&&sy!=1,sameoutsidex=sx==tx&&sx!=1;
                if(crossx&&!sameoutsidey){if(i==0)local_safe_[a*block_classes_+b]=0;else if(sy==1&&ty==1)local_cross_delay_[a*block_classes_+b]+=292;}
                if(crossy&&!sameoutsidex&&i==0)local_safe_[a*block_classes_+b]=0;
#ifdef SRB_VERTICAL_DETOUR_CACHE
                if(i>0&&std::abs(sy-ty)==2&&!(sx==tx&&sx!=1))local_vertical_detour_[a*block_classes_+b]=1;
#endif
#ifdef SRB_BLOCK_FOLD_CACHE
                if(i>0&&sy==1&&ty==1){int width=kBlockRight[i]-kBlockLeft[i]+1;if(sx==0&&tx==2)local_dx_fold_[a*block_classes_+b]-=int16_t(width);else if(sx==2&&tx==0)local_dx_fold_[a*block_classes_+b]+=int16_t(width);}
#endif
            }block_effect_[a*block_classes_+b]=v;}
    }
#endif
    float sqrt_distance_[550] = {};
#ifndef SRB_FAST_PORT_DECODE
    int16_t port_slots_[2048];
    uint16_t port_lengths_[srb_fast_data::kPortCount];

    static uint32_t hash_port(Slice name) {
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < name.size; ++i) h = (h ^ static_cast<unsigned char>(name.data[i])) * 16777619u;
        return h;
    }
#endif

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

#ifdef SRB_FAST_COORD_DECODE
    // Coordinates are bounded to x<120/y<550, hence each field has at most
    // three digits.  A fixed three-step decoder removes the generic overflow
    // loop from two billion endpoint parses without storing any lookup table.
    static bool parse_grid_coordinate(Slice text, size_t& pos, char separator,
                                      unsigned limit, int& value) {
        if (pos >= text.size) return false;
        unsigned digit = static_cast<unsigned>(text.data[pos] - '0');
        if (digit > 9) return false;
        unsigned result = digit;
        ++pos;
        if (pos < text.size && text.data[pos] != separator) {
            digit = static_cast<unsigned>(text.data[pos] - '0');
            if (digit > 9) return false;
            result = result * 10 + digit;
            ++pos;
            if (pos < text.size && text.data[pos] != separator) {
                digit = static_cast<unsigned>(text.data[pos] - '0');
                if (digit > 9) return false;
                result = result * 10 + digit;
                ++pos;
            }
        }
        if (pos >= text.size || text.data[pos++] != separator || result >= limit)
            return false;
        value = static_cast<int>(result);
        return true;
    }
#endif

    static int compare(Slice a, const char* b) {
        const size_t bn = std::strlen(b);
        const size_t n = std::min(a.size, bn);
        int c = std::memcmp(a.data, b, n);
        if (c != 0) return c;
        if (a.size < bn) return -1;
        if (a.size > bn) return 1;
        return 0;
    }

#ifndef SRB_FAST_PORT_DECODE
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
#endif

#ifdef SRB_FAST_PORT_DECODE
    // The contest architecture has a fixed, highly regular 496-port alphabet:
    //   A_/S_: 8 letters x 11 suffixes
    //   I/Z D/L/Q: 3 wire classes x 4 directions x 8 lanes
    //   I/Z S: 4 directions x 2 variants x 8 lanes
    // Decode that grammar directly instead of hashing 2 billion endpoint port
    // strings on a billion-query run.  Every byte is still validated, so bad
    // input keeps the same parse-failure behaviour as the generic hash table.
    static int find_port_structured(Slice name) {
        if (name.size == 4 || name.size == 5) {
            const char kind = name.data[0];
            if ((kind != 'A' && kind != 'S') || name.data[1] != '_') return -1;
            const unsigned group = static_cast<unsigned>(name.data[2] - 'A');
            if (group >= 8) return -1;
            int suffix = -1;
            if (name.size == 4) {
                const char tail = name.data[3];
                if (tail >= '0' && tail <= '5') suffix = tail - '0';
                else if (tail == 'D') suffix = 6;
                else if (tail == 'M') suffix = 7;
                else if (tail == 'Y') suffix = 10;
            } else if (name.data[3] == 'Q' &&
                       (name.data[4] == '0' || name.data[4] == '1')) {
                suffix = 8 + name.data[4] - '0';
            }
            if (suffix < 0) return -1;
            return (kind == 'A' ? 320 : 408) + int(group) * 11 + suffix;
        }

        if (name.size != 6 && name.size != 7) return -1;
        const char kind = name.data[0];
        if (kind != 'I' && kind != 'Z') return -1;
        const unsigned lane_pos = static_cast<unsigned>(name.size - 2);
        if (name.data[name.size - 3] != '[' || name.data[name.size - 1] != ']' ||
            name.data[lane_pos] < '0' || name.data[lane_pos] > '7') return -1;
        const int lane = name.data[lane_pos] - '0';
        const char family = name.data[1];
        const char direction = name.data[2];
        int direction_index = -1;
        if (direction == 'E') direction_index = 0;
        else if (direction == 'N') direction_index = 1;
        else if (direction == 'S') direction_index = 2;
        else if (direction == 'W') direction_index = 3;
        if (direction_index < 0) return -1;

        if (name.size == 6) {
            int base = -1;
            if (family == 'D') base = kind == 'I' ? 160 : 128;
            else if (family == 'L') base = kind == 'I' ? 288 : 256;
            else if (family == 'Q') base = kind == 'I' ? 224 : 192;
            if (base < 0) return -1;
            return base + direction_index * 8 + lane;
        }

        if (family != 'S' || (name.data[3] != 'A' && name.data[3] != 'B')) return -1;
        const int base = kind == 'I' ? 64 : 0;
        return base + direction_index * 16 + (name.data[3] == 'B' ? 8 : 0) + lane;
    }
#endif

    static int side(int value, int low, int high) {
        if (value < low) return 0;
        if (value > high) return 2;
        return 1;
    }
};

}  // namespace srb_fast
