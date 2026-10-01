#pragma once

#include "../plusone-srb_fast_v3/srb_fast_v3/local_arch_data.hpp"
#include "../plusone-srb_fast_v3/srb_fast_v3/srb_fast.hpp"
#include "v4_residual_data.hpp"
#if defined(SRB_V6_ENABLE_MACRO8_ENDPOINT) && defined(SRB_V6_ENABLE_MACRO10_FAMILY_PHASE)
#error "select only one experimental V6 macro residual"
#elif (defined(SRB_V4_ENABLE_FAMILY_COMPACT) && defined(SRB_V4_ENABLE_FAMILY_RESIDUAL)) || \
    ((defined(SRB_V6_ENABLE_MACRO8_ENDPOINT) || \
      defined(SRB_V6_ENABLE_MACRO10_FAMILY_PHASE)) && \
     (defined(SRB_V4_ENABLE_FAMILY_COMPACT) || defined(SRB_V4_ENABLE_FAMILY_RESIDUAL)))
#error "select only one experimental V4 family residual"
#elif defined(SRB_V6_ENABLE_MACRO10_FAMILY_PHASE)
#include "../srb_fast_v6/v6_macro10_data.hpp"
#define SRB_V4_HAS_FAMILY_RESIDUAL 1
#define SRB_V6_HAS_MACRO 1
#elif defined(SRB_V6_ENABLE_MACRO8_ENDPOINT)
#include "../srb_fast_v6/v6_macro8_data.hpp"
#define SRB_V4_HAS_FAMILY_RESIDUAL 1
#define SRB_V6_HAS_MACRO 1
#elif defined(SRB_V4_ENABLE_FAMILY_COMPACT)
#include "experiments/v4_family_compact_data.hpp"
#define SRB_V4_HAS_FAMILY_RESIDUAL 1
#elif defined(SRB_V4_ENABLE_FAMILY_RESIDUAL)
#include "experiments/v4_family_residual_data.hpp"
#define SRB_V4_HAS_FAMILY_RESIDUAL 1
#endif

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace srb_v4 {

enum class ResidualKernel : uint8_t {
    Reference,
    Optimized,
#ifdef SRB_V4_HAS_FAMILY_RESIDUAL
    Family,
#endif
};

static constexpr uint32_t kQueryRadius = 48;
static constexpr uint32_t kRequiredAtlasRadiusForV3Parity = 56;
static constexpr uint32_t kInf = std::numeric_limits<uint32_t>::max();

enum class Branch : uint8_t {
    SameEndpoint = 0,
    LegacyAtlas = 1,
    V3Fallback = 2,
    V4Residual = 3,
};

struct Prediction {
    uint32_t delay = 0;
    Branch branch = Branch::V3Fallback;
};

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
    void load(const std::string& path, bool verify_fingerprint) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) throw std::runtime_error("cannot open local Atlas: " + path);
        const std::streamoff file_size = input.tellg();
        input.seekg(0, std::ios::beg);
        AtlasHeader header{};
        input.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!input || std::memcmp(header.magic, "SRBLAT2", 7) != 0 ||
            header.version != 1 || header.routes != srb_local_arch::kRouteCount ||
            header.sources != srb_local_arch::kRouteCount ||
            header.radius < kRequiredAtlasRadiusForV3Parity ||
            header.cells != (header.radius * 2 + 1) * (header.radius * 2 + 1) ||
            header.header_bytes != sizeof(AtlasHeader)) {
            throw std::runtime_error("invalid or incompatible local Atlas header: " + path);
        }
        const uint64_t expected_values =
            static_cast<uint64_t>(header.sources) * header.cells * header.routes;
        const uint64_t expected_bytes = sizeof(AtlasHeader) + expected_values * sizeof(uint16_t);
        if (header.values != expected_values || expected_values > std::numeric_limits<size_t>::max() ||
            file_size < 0 || static_cast<uint64_t>(file_size) != expected_bytes) {
            throw std::runtime_error("invalid local Atlas size: " + path);
        }
        radius_ = static_cast<int>(header.radius);
        width_ = radius_ * 2 + 1;
        cells_ = header.cells;
        values_.resize(static_cast<size_t>(header.values));
        input.read(reinterpret_cast<char*>(values_.data()),
                   static_cast<std::streamsize>(values_.size() * sizeof(uint16_t)));
        if (!input) throw std::runtime_error("truncated local Atlas: " + path);
        if (verify_fingerprint) {
            uint32_t crc = UINT32_C(0xFFFFFFFF);
            crc = crc32_update(crc, reinterpret_cast<const uint8_t*>(&header), sizeof(header));
            crc = crc32_update(
                crc,
                reinterpret_cast<const uint8_t*>(values_.data()),
                values_.size() * sizeof(uint16_t));
            crc ^= UINT32_C(0xFFFFFFFF);
            if (crc != srb_v4_data::kAtlasCrc32) {
                throw std::runtime_error(
                    "Atlas fingerprint mismatch: expected model/architecture-compatible local_atlas.bin");
            }
        }
        path_ = path;
    }

    uint16_t get(uint16_t source_route, int dx, int dy, uint16_t target_route) const {
        if (dx < -radius_ || dx > radius_ || dy < -radius_ || dy > radius_) return UINT16_MAX;
        const uint32_t cell = static_cast<uint32_t>((dy + radius_) * width_ + (dx + radius_));
        const uint64_t index =
            (static_cast<uint64_t>(source_route) * cells_ + cell) *
                srb_local_arch::kRouteCount +
            target_route;
        return values_[static_cast<size_t>(index)];
    }

    size_t memory_bytes() const { return values_.size() * sizeof(uint16_t); }
    const std::string& path() const { return path_; }
    bool loaded() const { return !values_.empty(); }
    int radius() const { return radius_; }

private:
    static uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t size) {
        static uint32_t table[256] = {};
        static bool initialized = false;
        if (!initialized) {
            for (uint32_t value = 0; value < 256; ++value) {
                uint32_t entry = value;
                for (int bit = 0; bit < 8; ++bit) {
                    entry = (entry >> 1) ^
                            (UINT32_C(0xEDB88320) & static_cast<uint32_t>(-
                                static_cast<int32_t>(entry & 1)));
                }
                table[value] = entry;
            }
            initialized = true;
        }
        for (size_t index = 0; index < size; ++index) {
            crc = table[(crc ^ data[index]) & 0xFFu] ^ (crc >> 8);
        }
        return crc;
    }

    int radius_ = 0;
    int width_ = 0;
    uint32_t cells_ = 0;
    std::vector<uint16_t> values_;
    std::string path_;
};

class Estimator {
public:
    Estimator(const std::string& atlas_path,
              bool enable_atlas,
              bool enable_residual,
              bool verify_atlas_fingerprint,
              bool use_fused_fallback = true,
              ResidualKernel residual_kernel = ResidualKernel::Optimized,
              int atlas_query_radius = static_cast<int>(kQueryRadius))
        : enable_atlas_(enable_atlas),
          enable_residual_(enable_residual),
          use_fused_fallback_(use_fused_fallback),
          residual_kernel_(residual_kernel),
          atlas_query_radius_(atlas_query_radius) {
        if (atlas_query_radius_ < 0 || atlas_query_radius_ > 56) {
            throw std::runtime_error("Atlas query radius must be in [0,56]");
        }
        for (int distance = 0; distance < 550; ++distance) {
            sqrt_distance_[distance] = std::sqrt(static_cast<float>(distance));
            y_clearance_[distance] = static_cast<uint8_t>(
                std::min(119, std::min(distance, 549 - distance)));
        }
        for (int distance = 0; distance < 120; ++distance) {
            x_clearance_[distance] = static_cast<uint8_t>(std::min(distance, 119 - distance));
            int bin = 0;
            static const int edges[9] = {0, 1, 2, 4, 8, 16, 32, 64, 128};
            while (bin < 9 && distance >= edges[bin]) ++bin;
            boundary_bin_by_clearance_[distance] = static_cast<uint8_t>(bin);
        }
#ifdef SRB_V6_HAS_MACRO
#ifdef SRB_V6_ENABLE_MACRO10_FAMILY_PHASE
        static constexpr int macro_period = 10;
#else
        static constexpr int macro_period = 8;
#endif
        for (int value = 0; value < 120; ++value) {
            x_phase_[value] = static_cast<uint8_t>(value % macro_period);
        }
        for (int value = 0; value < 550; ++value) {
            y_phase_[value] = static_cast<uint8_t>(value % macro_period);
        }
        for (int value = -119; value <= 119; ++value) {
            dx_remainder_[value + 119] = static_cast<uint8_t>(
                (value % macro_period + macro_period) % macro_period);
        }
        for (int value = -549; value <= 549; ++value) {
            dy_remainder_[value + 549] = static_cast<uint8_t>(
                (value % macro_period + macro_period) % macro_period);
        }
#endif
        if (enable_atlas_) {
            atlas_.load(atlas_path, verify_atlas_fingerprint);
            if (atlas_query_radius_ > atlas_.radius()) {
                throw std::runtime_error("Atlas query radius exceeds loaded Atlas radius");
            }
        }
    }

    bool predict_spec(srb_fast::Slice from, srb_fast::Slice to, Prediction& result) {
        srb_fast::Endpoint source, target;
        if (!fallback_.parse_endpoint(from, source) || !fallback_.parse_endpoint(to, target)) {
            return false;
        }
        if (source.x == target.x && source.y == target.y && source.port == target.port) {
            result.delay = 0;
            result.branch = Branch::SameEndpoint;
            ++same_endpoint_queries_;
            return true;
        }
        uint32_t local = 0;
        if (enable_atlas_ && predict_local(source, target, local)) {
            result.delay = local;
            result.branch = Branch::LegacyAtlas;
            ++atlas_queries_;
            return true;
        }
        const FallbackContext context =
            use_fused_fallback_ ? predict_fallback_with_context(source, target)
                                : FallbackContext{fallback_.predict(source, target), 0, 0, 0};
        const uint32_t base = context.delay;
        if (enable_residual_) {
#ifdef SRB_V4_HAS_FAMILY_RESIDUAL
            if (residual_kernel_ == ResidualKernel::Family) {
                result.delay = predict_residual_family(source, target, context);
            } else
#endif
            {
                result.delay = residual_kernel_ == ResidualKernel::Optimized
                                   ? predict_residual_optimized(source, target, context)
                                   : predict_residual_reference(source, target, context);
            }
            result.branch = Branch::V4Residual;
            ++residual_queries_;
        } else {
            result.delay = base;
            result.branch = Branch::V3Fallback;
            ++fallback_queries_;
        }
        return true;
    }

    uint64_t same_endpoint_queries() const { return same_endpoint_queries_; }
    uint64_t atlas_queries() const { return atlas_queries_; }
    uint64_t fallback_queries() const { return fallback_queries_; }
    uint64_t residual_queries() const { return residual_queries_; }
    size_t atlas_memory_bytes() const { return atlas_.memory_bytes(); }
    const std::string& atlas_path() const { return atlas_.path(); }
    bool atlas_enabled() const { return enable_atlas_; }
    bool residual_enabled() const { return enable_residual_; }

private:
    struct FallbackContext {
        uint32_t delay;
        uint8_t intersected_blocks;
        uint8_t direction;
        uint16_t chebyshev;
    };

    struct Seed {
        uint16_t route;
        int8_t dx;
        int8_t dy;
        uint16_t cost;
    };

    static bool safe_regular_region(const srb_fast::Endpoint& source,
                                    const srb_fast::Endpoint& target) {
        const int min_x = std::min<int>(source.x, target.x);
        const int max_x = std::max<int>(source.x, target.x);
        const int min_y = std::min<int>(source.y, target.y);
        const int max_y = std::max<int>(source.y, target.y);
        for (int block = 0; block < srb_fast_data::kBlockCount; ++block) {
            if (min_x <= srb_fast_data::kBlockRight[block] &&
                max_x >= srb_fast_data::kBlockLeft[block] &&
                min_y <= srb_fast_data::kBlockUpper[block] &&
                max_y >= srb_fast_data::kBlockLower[block]) {
                return false;
            }
        }
        return true;
    }

    static uint32_t direct_arc(uint16_t source_input, uint16_t target_port) {
        using namespace srb_local_arch;
        for (uint32_t index = kDirectOffset[source_input];
             index < kDirectOffset[source_input + 1]; ++index) {
            if (kDirectArcs[index].id == target_port) return kDirectArcs[index].cost;
        }
        return kInf;
    }

    bool predict_local(const srb_fast::Endpoint& source,
                       const srb_fast::Endpoint& target,
                       uint32_t& result) const {
        using namespace srb_local_arch;
        const int dx = static_cast<int>(target.x) - source.x;
        const int dy = static_cast<int>(target.y) - source.y;
        if (std::max(std::abs(dx), std::abs(dy)) > atlas_query_radius_) return false;
        if (!safe_regular_region(source, target)) return false;

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

        uint32_t best = kInf;
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
                const uint16_t middle = atlas_.get(
                    seed.route, residual_dx, residual_dy, static_cast<uint16_t>(target_route));
                if (middle != UINT16_MAX) {
                    best = std::min(best, static_cast<uint32_t>(seed.cost) + middle);
                }
            } else {
                for (uint32_t index = kTargetOffset[target.port];
                     index < kTargetOffset[target.port + 1]; ++index) {
                    const PortCost& terminal = kTargetArcs[index];
                    const uint16_t middle =
                        atlas_.get(seed.route, residual_dx, residual_dy, terminal.id);
                    if (middle != UINT16_MAX) {
                        best = std::min(
                            best,
                            static_cast<uint32_t>(seed.cost) + middle + terminal.cost);
                    }
                }
            }
        }
        if (best == kInf) return false;
        best += std::abs(static_cast<int>(srb_fast_data::kXGapPrefix[target.x]) -
                         static_cast<int>(srb_fast_data::kXGapPrefix[source.x]));
        best += std::abs(static_cast<int>(srb_fast_data::kYGapPrefix[target.y]) -
                         static_cast<int>(srb_fast_data::kYGapPrefix[source.y]));
        result = best;
        return true;
    }

    static int direction_code(int dx, int dy) {
        return (dx > 0 ? 1 : 0) + (dx < 0 ? 2 : 0) +
               (dy > 0 ? 3 : 0) + (dy < 0 ? 6 : 0);
    }

    static int boundary_bin(const srb_fast::Endpoint& source,
                            const srb_fast::Endpoint& target) {
        int clearance = std::min<int>(source.x, source.y);
        clearance = std::min(clearance, static_cast<int>(target.x));
        clearance = std::min(clearance, static_cast<int>(target.y));
        clearance = std::min(clearance, 119 - static_cast<int>(source.x));
        clearance = std::min(clearance, 549 - static_cast<int>(source.y));
        clearance = std::min(clearance, 119 - static_cast<int>(target.x));
        clearance = std::min(clearance, 549 - static_cast<int>(target.y));
        static const int edges[9] = {0, 1, 2, 4, 8, 16, 32, 64, 128};
        int bin = 0;
        while (bin < 9 && clearance >= edges[bin]) ++bin;
        return bin;
    }

    int fast_boundary_bin(const srb_fast::Endpoint& source,
                          const srb_fast::Endpoint& target) const {
        const int source_clearance = std::min<int>(
            x_clearance_[source.x], y_clearance_[source.y]);
        const int target_clearance = std::min<int>(
            x_clearance_[target.x], y_clearance_[target.y]);
        return boundary_bin_by_clearance_[std::min(source_clearance, target_clearance)];
    }

    static int side(int value, int low, int high) {
        if (value < low) return 0;
        if (value > high) return 2;
        return 1;
    }

    FallbackContext predict_fallback_with_context(const srb_fast::Endpoint& source,
                                                   const srb_fast::Endpoint& target) const {
        using namespace srb_fast_data;
        const int dx = static_cast<int>(target.x) - source.x;
        const int dy = static_cast<int>(target.y) - source.y;
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

        const int quad = direction_code(dx, dy);
        value += kSrcQuad[static_cast<size_t>(source.port) * 9 + quad];
        value += kDstQuad[static_cast<size_t>(target.port) * 9 + quad];
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
        value += kPortPair[static_cast<size_t>(source.port) * kPortCount + target.port];
        value += kSrcDistance[(static_cast<size_t>(source.port) * 9 + quad) * 16 + distance_bin];
        value += kDstDistance[(static_cast<size_t>(target.port) * 9 + quad) * 16 + distance_bin];
        value += kSrcRegion[(source.x / 8) * 69 + source.y / 8];
        value += kDstRegion[(target.x / 8) * 69 + target.y / 8];
        const int remainder = rx * 17 + ry;
        value += kSrcRemainder[static_cast<size_t>(source.port) * (17 * 17) + remainder];
        value += kDstRemainder[static_cast<size_t>(target.port) * (17 * 17) + remainder];
        value += kGeom4[quad * (30 * 138) + std::min(29, ax / 4) * 138 + std::min(137, ay / 4)];
        value += kDisplacement[static_cast<size_t>(dx + 119) * 1099 + (dy + 549)];

        const int min_x = std::min<int>(source.x, target.x);
        const int max_x = std::max<int>(source.x, target.x);
        const int min_y = std::min<int>(source.y, target.y);
        const int max_y = std::max<int>(source.y, target.y);
        int intersected_blocks = 0;
        for (int block = 0; block < kBlockCount; ++block) {
            const int code = side(source.x, kBlockLeft[block], kBlockRight[block])
                           + 3 * side(target.x, kBlockLeft[block], kBlockRight[block])
                           + 9 * side(source.y, kBlockLower[block], kBlockUpper[block])
                           + 27 * side(target.y, kBlockLower[block], kBlockUpper[block]);
            value += kBlockEffect[block * 81 + code];
            intersected_blocks += min_x <= kBlockRight[block] && max_x >= kBlockLeft[block] &&
                                  min_y <= kBlockUpper[block] && max_y >= kBlockLower[block];
        }
        value += std::abs(static_cast<int>(kXGapPrefix[target.x]) - kXGapPrefix[source.x]);
        value += std::abs(static_cast<int>(kYGapPrefix[target.y]) - kYGapPrefix[source.y]);

        uint32_t delay = 0;
        if (value > 0.0f) {
            const double rounded = std::floor(static_cast<double>(value) + 0.5);
            delay = rounded >= static_cast<double>(std::numeric_limits<uint32_t>::max())
                        ? std::numeric_limits<uint32_t>::max()
                        : static_cast<uint32_t>(rounded);
        }
        return FallbackContext{
            delay,
            static_cast<uint8_t>(std::min(intersected_blocks, 7)),
            static_cast<uint8_t>(quad),
            static_cast<uint16_t>(std::max(ax, ay))};
    }

    static uint32_t predict_residual_with_boundary(const srb_fast::Endpoint& source,
                                                   const srb_fast::Endpoint& target,
                                                   const FallbackContext& context,
                                                   int boundary_code) {
        const uint32_t base = context.delay;
        if (base == 0) return 0;
        const int distance_bin = std::min(17, static_cast<int>(context.chebyshev) / 32);
        const int quad = context.direction;
        const int prediction_bin = std::min<int>(67, base / 128);
        const int horizontal_gap_count = std::min(
            15,
            std::abs(static_cast<int>(srb_v4_data::kYGapCountPrefix[target.y]) -
                     static_cast<int>(srb_v4_data::kYGapCountPrefix[source.y])));
        const int vertical_gap_count = std::min(
            15,
            std::abs(static_cast<int>(srb_v4_data::kXGapCountPrefix[target.x]) -
                     static_cast<int>(srb_v4_data::kXGapCountPrefix[source.x])));
        const int gap_shape = horizontal_gap_count * 16 + vertical_gap_count;
        const int block_distance =
            (static_cast<int>(context.intersected_blocks) * 9 + quad) * 18 + distance_bin;
        const int distance_quad = quad * 18 + distance_bin;
        float correction = srb_v4_data::kPredictionBin[prediction_bin];
        correction += srb_v4_data::kDistanceQuad[distance_quad];
        correction += srb_v4_data::kBoundary[boundary_code];
        correction += srb_v4_data::kGapShape[gap_shape];
        correction += srb_v4_data::kBlockDistance[block_distance];
        correction += srb_v4_data::kSourcePort[source.port];
        correction += srb_v4_data::kTargetPort[target.port];
        correction *= srb_v4_data::kAlpha;
        const float value = static_cast<float>(base) * (1.0f + correction);
        if (!(value > 0.0f)) return 0;
        const double rounded = std::floor(static_cast<double>(value) + 0.5);
        if (rounded >= static_cast<double>(std::numeric_limits<uint32_t>::max())) {
            return std::numeric_limits<uint32_t>::max();
        }
        return static_cast<uint32_t>(rounded);
    }

    static uint32_t predict_residual_reference(const srb_fast::Endpoint& source,
                                               const srb_fast::Endpoint& target,
                                               const FallbackContext& context) {
        return predict_residual_with_boundary(
            source, target, context, boundary_bin(source, target));
    }

    uint32_t predict_residual_optimized(const srb_fast::Endpoint& source,
                                        const srb_fast::Endpoint& target,
                                        const FallbackContext& context) const {
        return predict_residual_with_boundary(
            source, target, context, fast_boundary_bin(source, target));
    }

#ifdef SRB_V4_HAS_FAMILY_RESIDUAL
    uint32_t predict_residual_family(const srb_fast::Endpoint& source,
                                     const srb_fast::Endpoint& target,
                                     const FallbackContext& context) const {
        using namespace srb_v4_family_data;
        const uint32_t base = context.delay;
        if (base == 0) return 0;
        const int distance_bin = std::min(17, static_cast<int>(context.chebyshev) / 32);
        const int quad = context.direction;
        const int prediction_bin = std::min<int>(67, base / 128);
        const int horizontal_gap_count = std::min(
            15,
            std::abs(static_cast<int>(kYGapCountPrefix[target.y]) -
                     static_cast<int>(kYGapCountPrefix[source.y])));
        const int vertical_gap_count = std::min(
            15,
            std::abs(static_cast<int>(kXGapCountPrefix[target.x]) -
                     static_cast<int>(kXGapCountPrefix[source.x])));
        const int gap_shape = horizontal_gap_count * 16 + vertical_gap_count;
        const int block_distance =
            (static_cast<int>(context.intersected_blocks) * 9 + quad) * 18 + distance_bin;
        const int distance_quad = quad * 18 + distance_bin;
        const uint16_t source_family = kPortToFamily[source.port];
        const uint16_t target_family = kPortToFamily[target.port];
        float correction = kPredictionBin[prediction_bin];
        correction += kDistanceQuad[distance_quad];
        correction += kBoundary[fast_boundary_bin(source, target)];
        correction += kGapShape[gap_shape];
        correction += kBlockDistance[block_distance];
        correction += kPortFamilyPair[
            static_cast<size_t>(source_family) * kFamilyCount + target_family];
 #ifdef SRB_V4_ENABLE_FAMILY_COMPACT
        correction += kSourceFamily[source_family];
        correction += kTargetFamily[target_family];
 #else
        correction += kSourceFamilyDistance[
            (static_cast<size_t>(source_family) * 9 + quad) * 18 + distance_bin];
        correction += kTargetFamilyDistance[
            (static_cast<size_t>(target_family) * 9 + quad) * 18 + distance_bin];
 #endif
#ifdef SRB_V6_HAS_MACRO
        const int dx = static_cast<int>(target.x) - source.x;
        const int dy = static_cast<int>(target.y) - source.y;
#ifdef SRB_V6_ENABLE_MACRO10_FAMILY_PHASE
        static constexpr int kMacroPeriod = 10;
#else
        static constexpr int kMacroPeriod = 8;
#endif
        static constexpr int kMacroCells = kMacroPeriod * kMacroPeriod;
        const int remainder = dx_remainder_[dx + 119] * kMacroPeriod +
                              dy_remainder_[dy + 549];
        correction += kMacro8Direction[quad * kMacroCells + remainder];
        correction += kSourceFamilyMacro8[
            static_cast<size_t>(source_family) * kMacroCells + remainder];
        correction += kTargetFamilyMacro8[
            static_cast<size_t>(target_family) * kMacroCells + remainder];
#ifdef SRB_V6_ENABLE_MACRO10_FAMILY_PHASE
        const int source_phase = x_phase_[source.x] * kMacroPeriod + y_phase_[source.y];
        const int target_phase = x_phase_[target.x] * kMacroPeriod + y_phase_[target.y];
        correction += kSourcePhase[source_phase];
        correction += kTargetPhase[target_phase];
        correction += kSourceFamilyPhase[
            static_cast<size_t>(source_family) * kMacroCells + source_phase];
        correction += kTargetFamilyPhase[
            static_cast<size_t>(target_family) * kMacroCells + target_phase];
#endif
#endif
        correction *= kAlpha;
        const float value = static_cast<float>(base) * (1.0f + correction);
        if (!(value > 0.0f)) return 0;
        const double rounded = std::floor(static_cast<double>(value) + 0.5);
        if (rounded >= static_cast<double>(std::numeric_limits<uint32_t>::max())) {
            return std::numeric_limits<uint32_t>::max();
        }
        return static_cast<uint32_t>(rounded);
    }
#endif

    srb_fast::FastEstimator fallback_;
    LocalAtlas atlas_;
    float sqrt_distance_[550] = {};
    uint8_t x_clearance_[120] = {};
    uint8_t y_clearance_[550] = {};
    uint8_t boundary_bin_by_clearance_[120] = {};
#ifdef SRB_V6_HAS_MACRO
    uint8_t x_phase_[120] = {};
    uint8_t y_phase_[550] = {};
    uint8_t dx_remainder_[239] = {};
    uint8_t dy_remainder_[1099] = {};
#endif
    bool enable_atlas_ = true;
    bool enable_residual_ = true;
    bool use_fused_fallback_ = true;
    ResidualKernel residual_kernel_ = ResidualKernel::Optimized;
    int atlas_query_radius_ = static_cast<int>(kQueryRadius);
    uint64_t same_endpoint_queries_ = 0;
    uint64_t atlas_queries_ = 0;
    uint64_t fallback_queries_ = 0;
    uint64_t residual_queries_ = 0;
};

}  // namespace srb_v4
