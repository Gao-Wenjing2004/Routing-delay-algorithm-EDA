#pragma once

#include "local_arch_data.hpp"
#include "srb_fast.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>

extern "C" {
extern const unsigned char p14_atlas_begin[];
extern const unsigned char p14_atlas_end[];
extern const unsigned char p14_atlas_exceptions_begin[];
extern const unsigned char p14_atlas_exceptions_end[];
}

namespace p14_runtime {

class PackedAtlas {
public:
    static constexpr int kQueryRadius = 26;
    static constexpr int kAtlasRadius = 26;
    static constexpr int kWidth = kAtlasRadius * 2 + 1;
    static constexpr uint32_t kCells = kWidth * kWidth;
    static constexpr uint32_t kRoutes = srb_local_arch::kRouteCount;
    static constexpr uint32_t kBits = 10;
    static constexpr uint32_t kSentinel = (1u << kBits) - 1;
    static constexpr uint32_t kExceptionCount = 61599;
    static constexpr uint64_t kValues =
        static_cast<uint64_t>(kRoutes) * kCells * kRoutes;
    static constexpr uint64_t kBytes = (kValues * kBits + 7) / 8;

    PackedAtlas() {
        if (static_cast<uint64_t>(p14_atlas_end - p14_atlas_begin) != kBytes) {
            throw std::runtime_error("invalid embedded P14 Atlas size");
        }
        if (static_cast<uint64_t>(p14_atlas_exceptions_end - p14_atlas_exceptions_begin) !=
            static_cast<uint64_t>(kExceptionCount) * 6) {
            throw std::runtime_error("invalid embedded P14 Atlas exception size");
        }
    }

    bool query(const srb_fast::Endpoint& source,
               const srb_fast::Endpoint& target,
               uint32_t& result) const {
        using namespace srb_local_arch;
        const int dx = static_cast<int>(target.x) - source.x;
        const int dy = static_cast<int>(target.y) - source.y;
        if (std::max(std::abs(dx), std::abs(dy)) > kQueryRadius) return false;

        Seed seeds[64];
        int seed_count = 0;
        const int16_t source_input = kPortToInput[source.port];
        if (source_input >= 0) {
            const int16_t source_route = kInputToRoute[source_input];
            if (source_route >= 0) {
                seeds[seed_count++] = Seed{static_cast<uint16_t>(source_route), 0, 0, 0};
            } else {
                for (uint32_t index = kTransitionOffset[source_input];
                     index < kTransitionOffset[source_input + 1] && seed_count < 64; ++index) {
                    const Transition& edge = kTransitions[index];
                    seeds[seed_count++] = Seed{edge.next_route, edge.dx, edge.dy, edge.cost};
                }
            }
        } else {
            const OutputNet& net = kOutputNet[source.port];
            if (net.next_route < 0) return false;
            seeds[seed_count++] =
                Seed{static_cast<uint16_t>(net.next_route), net.dx, net.dy, 0};
        }
        if (seed_count == 0) return false;
        // The legacy V3 comment claimed a maximum source-Net span of 8, but
        // the generated architecture contains 10/12-cell Nets.  Never price
        // a query from a partial seed set: an omitted seed could be cheaper.
        for (int seed_index = 0; seed_index < seed_count; ++seed_index) {
            if (std::abs(dx - seeds[seed_index].dx) > kAtlasRadius ||
                std::abs(dy - seeds[seed_index].dy) > kAtlasRadius) {
                return false;
            }
        }

        uint32_t best = std::numeric_limits<uint32_t>::max();
        const int16_t target_input = kPortToInput[target.port];
        int16_t target_route = -1;
        if (target_input >= 0) {
            target_route = kInputToRoute[target_input];
            if (target_route < 0) return false;
        }
        if (dx == 0 && dy == 0 && source_input >= 0 && target_input < 0) {
            best = direct_arc(static_cast<uint16_t>(source_input), target.port);
        }
        for (int seed_index = 0; seed_index < seed_count; ++seed_index) {
            const Seed& seed = seeds[seed_index];
            const int residual_dx = dx - seed.dx;
            const int residual_dy = dy - seed.dy;
            if (target_route >= 0) {
                const uint16_t middle = get(
                    seed.route, residual_dx, residual_dy,
                    static_cast<uint16_t>(target_route));
                if (middle != std::numeric_limits<uint16_t>::max()) {
                    best = std::min(best, static_cast<uint32_t>(seed.cost) + middle);
                }
            } else {
                for (uint32_t index = kTargetOffset[target.port];
                     index < kTargetOffset[target.port + 1]; ++index) {
                    const PortCost& terminal = kTargetArcs[index];
                    const uint16_t middle =
                        get(seed.route, residual_dx, residual_dy, terminal.id);
                    if (middle != std::numeric_limits<uint16_t>::max()) {
                        best = std::min(best, static_cast<uint32_t>(seed.cost) +
                            middle + terminal.cost);
                    }
                }
            }
        }
        if (best == std::numeric_limits<uint32_t>::max()) return false;
        best += std::abs(static_cast<int>(srb_fast_data::kXGapPrefix[target.x]) -
                         static_cast<int>(srb_fast_data::kXGapPrefix[source.x]));
        best += std::abs(static_cast<int>(srb_fast_data::kYGapPrefix[target.y]) -
                         static_cast<int>(srb_fast_data::kYGapPrefix[source.y]));
        result = best;
        return true;
    }

private:
    struct Seed {
        uint16_t route;
        int8_t dx;
        int8_t dy;
        uint16_t cost;
    };

    static uint32_t direct_arc(uint16_t source_input, uint16_t target_port) {
        using namespace srb_local_arch;
        for (uint32_t index = kDirectOffset[source_input];
             index < kDirectOffset[source_input + 1]; ++index) {
            if (kDirectArcs[index].id == target_port) return kDirectArcs[index].cost;
        }
        return std::numeric_limits<uint32_t>::max();
    }

    static uint16_t get(uint16_t source_route,
                        int dx,
                        int dy,
                        uint16_t target_route) {
        if (dx < -kAtlasRadius || dx > kAtlasRadius ||
            dy < -kAtlasRadius || dy > kAtlasRadius) {
            return std::numeric_limits<uint16_t>::max();
        }
        const uint32_t cell = static_cast<uint32_t>(
            (dy + kAtlasRadius) * kWidth + dx + kAtlasRadius);
        const uint64_t index =
            (static_cast<uint64_t>(source_route) * kCells + cell) * kRoutes + target_route;
        const uint64_t bit = index * kBits;
        const unsigned char* data = p14_atlas_begin + (bit >> 3);
        const uint32_t word = static_cast<uint32_t>(data[0]) |
                              (static_cast<uint32_t>(data[1]) << 8) |
                              (static_cast<uint32_t>(data[2]) << 16);
        const uint16_t value = static_cast<uint16_t>((word >> (bit & 7)) & kSentinel);
        if (value != kSentinel) return value;
        const uint32_t* indices =
            reinterpret_cast<const uint32_t*>(p14_atlas_exceptions_begin);
        uint32_t lower = 0;
        uint32_t upper = kExceptionCount;
        while (lower < upper) {
            const uint32_t middle = lower + ((upper - lower) >> 1);
            if (indices[middle] < index) lower = middle + 1;
            else upper = middle;
        }
        if (lower == kExceptionCount || indices[lower] != index) {
            return std::numeric_limits<uint16_t>::max();
        }
        const uint16_t* exceptions = reinterpret_cast<const uint16_t*>(
            p14_atlas_exceptions_begin + static_cast<uint64_t>(kExceptionCount) * 4);
        return exceptions[lower];
    }
};

}  // namespace p14_runtime
