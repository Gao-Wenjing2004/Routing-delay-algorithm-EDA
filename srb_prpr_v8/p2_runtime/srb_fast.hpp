#pragma once

#include "fast_model_data.hpp"

#include <algorithm>
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
    float sqrt_distance_[550] = {};

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

    static int compare(Slice a, const char* b) {
        const size_t bn = std::strlen(b);
        const size_t n = std::min(a.size, bn);
        int c = std::memcmp(a.data, b, n);
        if (c != 0) return c;
        if (a.size < bn) return -1;
        if (a.size > bn) return 1;
        return 0;
    }

    static int find_port(Slice name) {
        int lo = 0;
        int hi = srb_fast_data::kPortCount;
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            int c = compare(name, srb_fast_data::kSortedPortNames[mid]);
            if (c == 0) return srb_fast_data::kSortedPortIds[mid];
            if (c < 0) hi = mid;
            else lo = mid + 1;
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
