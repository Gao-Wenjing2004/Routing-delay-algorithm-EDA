#pragma once

#include "fast_model_data.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace srb_fast {

struct Slice {
    const char* data = nullptr;
    size_t size = 0;
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
#if !defined(SRB_FAST_BINARY_PORT_LOOKUP) && !defined(P23_DIRECT_PORT_DECODE)
        build_port_hash();
#endif
    }

    bool parse_endpoint(Slice text, Endpoint& out) const {
#ifdef P21_TRUST_ENDPOINTS
        // Official rows have already been split/trimmed by the two-column CSV
        // reader and contain only legal SRB coordinates and port names.
        // Keep the digit loop because x/y widths vary, but remove duplicated
        // prefix, separator, range and hash-membership checks.
        std::size_t pos = 4;
        int x = 0;
        while (text.data[pos] != '_') {
            x = x * 10 + (text.data[pos++] - '0');
        }
        ++pos;
        int y = 0;
        while (text.data[pos] != '/') {
            y = y * 10 + (text.data[pos++] - '0');
        }
        ++pos;
        const Slice port{text.data + pos, text.size - pos};
        out.x = static_cast<int16_t>(x);
        out.y = static_cast<int16_t>(y);
        out.port = static_cast<uint16_t>(find_port(port));
        return true;
#else
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
#endif
    }

    bool predict_spec(Slice from, Slice to, uint32_t& result) const {
        Endpoint a, b;
        if (!parse_endpoint(from, a) || !parse_endpoint(to, b)) return false;
        result = predict(a, b);
        return true;
    }

    uint32_t predict(const Endpoint& a, const Endpoint& b) const {
        using namespace srb_fast_data;
        if (a.x == b.x && a.y == b.y && a.port == b.port) return 0;

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

        value += std::abs(static_cast<int>(kXGapPrefix[b.x]) - kXGapPrefix[a.x]);
        value += std::abs(static_cast<int>(kYGapPrefix[b.y]) - kYGapPrefix[a.y]);

        if (!(value > 0.0f)) return 0;
        const double rounded = std::floor(static_cast<double>(value) + 0.5);
        if (rounded >= static_cast<double>(std::numeric_limits<uint32_t>::max())) {
            return std::numeric_limits<uint32_t>::max();
        }
        return static_cast<uint32_t>(rounded);
    }

private:
#ifdef P18_PERFECT_PORT_HASH
    static constexpr std::size_t kPortHashSize = 16384;
    static constexpr uint64_t kPortHashSalt = UINT64_C(349);
#else
    static constexpr std::size_t kPortHashSize = 1024;
#endif
    float sqrt_distance_[550] = {};
#if !defined(P18_PERFECT_PORT_HASH) || !defined(P18_TRUST_PORTS)
    std::array<uint64_t, kPortHashSize> port_keys_{};
#endif
    std::array<uint16_t, kPortHashSize> port_ids_{};

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
            result = result * 10 + (text.data[pos] - '0');
            ++pos;
        }
        value = result;
        return true;
    }

    static uint64_t pack_port(Slice value) {
        if (!value.size || value.size > 7) return 0;
        uint64_t key = static_cast<uint64_t>(value.size) << 56;
        switch (value.size) {
        case 7: key |= static_cast<uint64_t>(static_cast<unsigned char>(value.data[6])) << 48;
                [[fallthrough]];
        case 6: key |= static_cast<uint64_t>(static_cast<unsigned char>(value.data[5])) << 40;
                [[fallthrough]];
        case 5: key |= static_cast<uint64_t>(static_cast<unsigned char>(value.data[4])) << 32;
                [[fallthrough]];
        case 4: key |= static_cast<uint64_t>(static_cast<unsigned char>(value.data[3])) << 24;
                [[fallthrough]];
        case 3: key |= static_cast<uint64_t>(static_cast<unsigned char>(value.data[2])) << 16;
                [[fallthrough]];
        case 2: key |= static_cast<uint64_t>(static_cast<unsigned char>(value.data[1])) << 8;
                [[fallthrough]];
        case 1: key |= static_cast<uint64_t>(static_cast<unsigned char>(value.data[0]));
                break;
        default: return 0;
        }
        return key;
    }

    static std::size_t port_slot(uint64_t key) {
#ifdef P18_PERFECT_PORT_HASH
        key ^= kPortHashSalt;
#endif
        key ^= key >> 33;
        key *= UINT64_C(0xff51afd7ed558ccd);
        key ^= key >> 33;
        return static_cast<std::size_t>(key) & (kPortHashSize - 1);
    }

#ifdef P23_DIRECT_PORT_DECODE
    static int find_port(Slice name) {
        // All official names belong to one of four regular families.  P21 has
        // already opted into trusted legal input, so decode the architecture
        // grammar directly instead of packing, hashing and touching a sparse
        // 32 KiB table twice per row.
        const char first = name.data[0];
        if (name.data[1] == '_') {
            // A_[A-H]{0..5,D,M,Q0,Q1,Y} and S_[...], 88 ports each.
            const int group = name.data[2] - 'A';
            const char suffix = name.data[3];
            int variant;
            if (suffix <= '5') variant = suffix - '0';
            else if (suffix == 'D') variant = 6;
            else if (suffix == 'M') variant = 7;
            else if (suffix == 'Q') variant = 8 + (name.data[4] - '0');
            else variant = 10;  // Y
            return (first == 'A' ? 320 : 408) + group * 11 + variant;
        }

        const char family = name.data[1];
        int base;
        if (first == 'I') {
            base = family == 'D' ? 160 : (family == 'L' ? 288 :
                   (family == 'Q' ? 224 : 64));
        } else {
            base = family == 'D' ? 128 : (family == 'L' ? 256 :
                   (family == 'Q' ? 192 : 0));
        }
        const char direction = name.data[2];
        const int direction_index = direction == 'E' ? 0 : (direction == 'N' ? 1 :
                                    (direction == 'S' ? 2 : 3));
        if (family != 'S') {
            return base + direction_index * 8 + (name.data[4] - '0');
        }
        const int ab = name.data[3] - 'A';
        return base + (direction_index * 2 + ab) * 8 + (name.data[5] - '0');
    }
#elif defined(SRB_FAST_BINARY_PORT_LOOKUP)
    static int compare_port(Slice value, const char* expected) {
        const std::size_t expected_size = std::strlen(expected);
        const std::size_t common = std::min(value.size, expected_size);
        const int compared = std::memcmp(value.data, expected, common);
        if (compared != 0) return compared;
        if (value.size < expected_size) return -1;
        if (value.size > expected_size) return 1;
        return 0;
    }

    int find_port(Slice name) const {
        int lower = 0;
        int upper = srb_fast_data::kPortCount;
        while (lower < upper) {
            const int middle = lower + (upper - lower) / 2;
            const int compared = compare_port(name, srb_fast_data::kSortedPortNames[middle]);
            if (compared == 0) return srb_fast_data::kSortedPortIds[middle];
            if (compared < 0) upper = middle;
            else lower = middle + 1;
        }
        return -1;
    }
#else
    void build_port_hash() {
        static_assert((kPortHashSize & (kPortHashSize - 1)) == 0,
                      "port hash size must be a power of two");
        for (uint16_t sorted = 0; sorted < srb_fast_data::kPortCount; ++sorted) {
            const char* text = srb_fast_data::kSortedPortNames[sorted];
            const Slice name{text, std::strlen(text)};
            const uint64_t key = pack_port(name);
            std::size_t slot = port_slot(key);
#ifndef P18_PERFECT_PORT_HASH
            while (port_keys_[slot]) slot = (slot + 1) & (kPortHashSize - 1);
#endif
#if !defined(P18_PERFECT_PORT_HASH) || !defined(P18_TRUST_PORTS)
            port_keys_[slot] = key;
#endif
            port_ids_[slot] = srb_fast_data::kSortedPortIds[sorted];
        }
    }

    int find_port(Slice name) const {
        const uint64_t key = pack_port(name);
        if (!key) return -1;
        std::size_t slot = port_slot(key);
#ifdef P18_PERFECT_PORT_HASH
#ifdef P18_TRUST_PORTS
        // Contest inputs only contain legal architecture ports.  The offline
        // selected salt is collision-free for all 496 legal names, so one
        // indexed load is enough on the billion-row hot path.
        return port_ids_[slot];
#else
        return port_keys_[slot] == key ? port_ids_[slot] : -1;
#endif
#else
        for (std::size_t probes = 0; probes < kPortHashSize; ++probes) {
            if (!port_keys_[slot]) return -1;
            if (port_keys_[slot] == key) return port_ids_[slot];
            slot = (slot + 1) & (kPortHashSize - 1);
        }
        return -1;
#endif
    }
#endif

    static int side(int value, int low, int high) {
        if (value < low) return 0;
        if (value > high) return 2;
        return 1;
    }
};

}  // namespace srb_fast
