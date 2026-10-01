#pragma once

#include "local_arch_data.hpp"
#include "srb_fast.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace srb_v3 {

static constexpr uint32_t kQueryRadius = 48;
static constexpr uint32_t kMaximumNetSpan = 8;
static constexpr uint32_t kRequiredAtlasRadius = kQueryRadius + kMaximumNetSpan;

#pragma pack(push, 1)
struct AtlasHeader {
    char magic[8];
    uint32_t version;
    uint32_t radius;
    uint32_t routes;
    uint32_t cells;
    uint32_t sources;
    uint32_t header_bytes;
    uint64_t values;
};
#pragma pack(pop)

class LocalAtlas {
public:
    void load(const std::string& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open local Atlas: " + path);
        AtlasHeader header{};
        input.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!input || std::memcmp(header.magic, "SRBLAT2", 7) != 0 ||
            header.version != 1 || header.routes != srb_local_arch::kRouteCount ||
            header.sources != srb_local_arch::kRouteCount ||
            header.radius < kRequiredAtlasRadius ||
            header.cells != (header.radius * 2 + 1) * (header.radius * 2 + 1) ||
            header.header_bytes != sizeof(AtlasHeader)) {
            throw std::runtime_error("invalid or incompatible local Atlas: " + path);
        }
        const uint64_t expected = static_cast<uint64_t>(header.sources) * header.cells * header.routes;
        if (header.values != expected || header.values > std::numeric_limits<size_t>::max()) {
            throw std::runtime_error("invalid local Atlas size");
        }
        radius_ = static_cast<int>(header.radius);
        width_ = radius_ * 2 + 1;
        cells_ = header.cells;
        values_.resize(static_cast<size_t>(header.values));
        input.read(reinterpret_cast<char*>(values_.data()),
                   static_cast<std::streamsize>(values_.size() * sizeof(uint16_t)));
        if (!input) throw std::runtime_error("truncated local Atlas: " + path);
        path_ = path;
    }

    uint16_t get(uint16_t source_route, int dx, int dy, uint16_t target_route) const {
        if (dx < -radius_ || dx > radius_ || dy < -radius_ || dy > radius_) return UINT16_MAX;
        const uint32_t cell = static_cast<uint32_t>((dy + radius_) * width_ + (dx + radius_));
        const uint64_t index = (static_cast<uint64_t>(source_route) * cells_ + cell)
                             * srb_local_arch::kRouteCount + target_route;
        return values_[static_cast<size_t>(index)];
    }

    size_t memory_bytes() const { return values_.size() * sizeof(uint16_t); }
    const std::string& path() const { return path_; }

private:
    int radius_ = 0;
    int width_ = 0;
    uint32_t cells_ = 0;
    std::vector<uint16_t> values_;
    std::string path_;
};

class HybridEstimator {
public:
    explicit HybridEstimator(const std::string& atlas_path) { atlas_.load(atlas_path); }

    bool predict_spec(srb_fast::Slice from, srb_fast::Slice to, uint32_t& result) {
        srb_fast::Endpoint a, b;
        if (!fallback_.parse_endpoint(from, a) || !fallback_.parse_endpoint(to, b)) return false;
        uint32_t local = 0;
        if (predict_local(a, b, local)) {
            result = local;
            ++local_queries_;
        } else {
            result = fallback_.predict(a, b);
            ++fallback_queries_;
        }
        return true;
    }

    uint64_t local_queries() const { return local_queries_; }
    uint64_t fallback_queries() const { return fallback_queries_; }
    size_t atlas_memory_bytes() const { return atlas_.memory_bytes(); }
    const std::string& atlas_path() const { return atlas_.path(); }

private:
    static constexpr uint32_t INF = std::numeric_limits<uint32_t>::max();

    struct Seed {
        uint16_t route;
        int8_t dx;
        int8_t dy;
        uint16_t cost;
    };

    bool safe_regular_region(const srb_fast::Endpoint& a, const srb_fast::Endpoint& b) const {
        // Boundaries proved safe on the public set: the exact local path normally
        // stays between its endpoints.  A Block is different because it changes
        // both Net landing coordinates and passability, so queries whose direct
        // endpoint box intersects a Block stay on the learned V1 fallback.
        const int min_x = std::min<int>(a.x, b.x);
        const int max_x = std::max<int>(a.x, b.x);
        const int min_y = std::min<int>(a.y, b.y);
        const int max_y = std::max<int>(a.y, b.y);
        for (int bi = 0; bi < srb_fast_data::kBlockCount; ++bi) {
            if (min_x <= srb_fast_data::kBlockRight[bi] &&
                max_x >= srb_fast_data::kBlockLeft[bi] &&
                min_y <= srb_fast_data::kBlockUpper[bi] &&
                max_y >= srb_fast_data::kBlockLower[bi]) {
                return false;
            }
        }
        return true;
    }

    static uint32_t direct_arc(uint16_t source_input, uint16_t target_port) {
        using namespace srb_local_arch;
        for (uint32_t i = kDirectOffset[source_input]; i < kDirectOffset[source_input + 1]; ++i) {
            if (kDirectArcs[i].id == target_port) return kDirectArcs[i].cost;
        }
        return INF;
    }

    bool predict_local(const srb_fast::Endpoint& a,
                       const srb_fast::Endpoint& b,
                       uint32_t& result) const {
        using namespace srb_local_arch;
        if (a.x == b.x && a.y == b.y && a.port == b.port) {
            result = 0;
            return true;
        }
        const int dx = static_cast<int>(b.x) - a.x;
        const int dy = static_cast<int>(b.y) - a.y;
        if (std::max(std::abs(dx), std::abs(dy)) > static_cast<int>(kQueryRadius)) return false;
        if (!safe_regular_region(a, b)) return false;

        Seed seeds[64];
        int seed_count = 0;
        const int16_t source_input = kPortToInput[a.port];
        if (source_input >= 0) {
            const int16_t source_route = kInputToRoute[source_input];
            if (source_route >= 0) {
                seeds[seed_count++] = Seed{static_cast<uint16_t>(source_route), 0, 0, 0};
            } else {
                for (uint32_t i = kTransitionOffset[source_input];
                     i < kTransitionOffset[source_input + 1] && seed_count < 64; ++i) {
                    const Transition& edge = kTransitions[i];
                    seeds[seed_count++] = Seed{edge.next_route, edge.dx, edge.dy, edge.cost};
                }
            }
        } else {
            const OutputNet& net = kOutputNet[a.port];
            if (net.next_route < 0) return false;
            seeds[seed_count++] = Seed{static_cast<uint16_t>(net.next_route), net.dx, net.dy, 0};
        }
        if (seed_count == 0) return false;

        uint32_t best = INF;
        const int16_t target_input = kPortToInput[b.port];
        int16_t target_route = -1;
        if (target_input >= 0) {
            target_route = kInputToRoute[target_input];
            if (target_route < 0) return false;
        }

        if (dx == 0 && dy == 0 && source_input >= 0 && target_input < 0) {
            best = direct_arc(static_cast<uint16_t>(source_input), b.port);
        }

        for (int si = 0; si < seed_count; ++si) {
            const Seed& seed = seeds[si];
            const int rdx = dx - seed.dx;
            const int rdy = dy - seed.dy;
            if (target_route >= 0) {
                const uint16_t middle = atlas_.get(seed.route, rdx, rdy,
                                                   static_cast<uint16_t>(target_route));
                if (middle != UINT16_MAX) best = std::min(best, static_cast<uint32_t>(seed.cost) + middle);
            } else {
                for (uint32_t ti = kTargetOffset[b.port]; ti < kTargetOffset[b.port + 1]; ++ti) {
                    const PortCost& terminal = kTargetArcs[ti];
                    const uint16_t middle = atlas_.get(seed.route, rdx, rdy, terminal.id);
                    if (middle == UINT16_MAX) continue;
                    best = std::min(best, static_cast<uint32_t>(seed.cost) + middle + terminal.cost);
                }
            }
        }
        if (best == INF) return false;

        best += std::abs(static_cast<int>(srb_fast_data::kXGapPrefix[b.x])
                       - static_cast<int>(srb_fast_data::kXGapPrefix[a.x]));
        best += std::abs(static_cast<int>(srb_fast_data::kYGapPrefix[b.y])
                       - static_cast<int>(srb_fast_data::kYGapPrefix[a.y]));
        result = best;
        return true;
    }

    srb_fast::FastEstimator fallback_;
    LocalAtlas atlas_;
    uint64_t local_queries_ = 0;
    uint64_t fallback_queries_ = 0;
};

}  // namespace srb_v3
