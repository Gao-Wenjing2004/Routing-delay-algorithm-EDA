#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace nocache_v10 {

// Immutable fixed-index model.  It contains no endpoint identity and never
// grows with requests, so it is a model table rather than a result cache.
class DirectResidual {
public:
    explicit DirectResidual(const std::string& path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) throw std::runtime_error("cannot open direct residual: " + path);
        const auto length = input.tellg();
        input.seekg(0);
        bytes_.resize(static_cast<std::size_t>(length));
        input.read(reinterpret_cast<char*>(bytes_.data()), static_cast<std::streamsize>(length));
        if (!input || bytes_.size() < kHeader || std::memcmp(bytes_.data(), "RSDV1001", 8))
            throw std::runtime_error("bad direct residual asset");
        const uint32_t version = u32(8);
        scale_ = u32(12);
        branches_ = u32(16); distances_ = u32(20); quadrants_ = u32(24);
        environments_ = u32(28); x_phases_ = u32(32); y_phases_ = u32(36);
        coarse_size_ = u32(40); fine_size_ = u32(44);
        if (version != 1 || branches_ != 10 || distances_ != 10 || quadrants_ != 9 ||
            environments_ != 4 || x_phases_ != 8 || y_phases_ != 12 ||
            bytes_.size() != kHeader + coarse_size_ + fine_size_)
            throw std::runtime_error("unsupported direct residual layout");
    }

    int correction(unsigned branch, int dx, int dy, int gap, unsigned block_mask) const {
        if (branch >= branches_) branch = branches_ - 1;
        const unsigned coarse = (((branch * distances_ + distance_bucket(dx, dy)) * quadrants_ +
            quadrant(dx, dy)) * environments_ + ((gap ? 1u : 0u) | (block_mask ? 2u : 0u)));
        const unsigned fine = (coarse * x_phases_ + unsigned(abs_i(dx)) % x_phases_) * y_phases_ +
                              unsigned(abs_i(dy)) % y_phases_;
        int8_t value = static_cast<int8_t>(bytes_[kHeader + coarse_size_ + fine]);
        if (value == -128) value = static_cast<int8_t>(bytes_[kHeader + coarse]);
        return value == -128 ? 0 : int(value) * int(scale_);
    }

    std::size_t memory_bytes() const { return bytes_.size(); }

private:
    static constexpr std::size_t kHeader = 48;
    std::vector<unsigned char> bytes_;
    uint32_t scale_ = 1, branches_ = 0, distances_ = 0, quadrants_ = 0;
    uint32_t environments_ = 0, x_phases_ = 0, y_phases_ = 0;
    uint32_t coarse_size_ = 0, fine_size_ = 0;

    uint32_t u32(std::size_t offset) const {
        uint32_t value;
        std::memcpy(&value, bytes_.data() + offset, sizeof(value));
        return value;
    }
    static int abs_i(int value) { return value < 0 ? -value : value; }
    static unsigned quadrant(int dx, int dy) {
        return (dx > 0 ? 1u : dx < 0 ? 2u : 0u) + (dy > 0 ? 3u : dy < 0 ? 6u : 0u);
    }
    static unsigned distance_bucket(int dx, int dy) {
        const int distance = abs_i(dx) > abs_i(dy) ? abs_i(dx) : abs_i(dy);
        if (distance <= 16) return 0; if (distance <= 23) return 1;
        if (distance == 24) return 2; if (distance <= 26) return 3;
        if (distance <= 48) return 4; if (distance <= 71) return 5;
        if (distance == 72) return 6; if (distance <= 128) return 7;
        if (distance <= 256) return 8; return 9;
    }
};

}  // namespace nocache_v10
