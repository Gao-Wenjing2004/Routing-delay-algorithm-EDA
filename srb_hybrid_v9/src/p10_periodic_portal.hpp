#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace p10 {

class PackedPortalClosureView {
public:
    PackedPortalClosureView(const unsigned char* bytes, std::size_t size)
        : bytes_(bytes), size_(size) {
        if (!bytes_ || size_ < 20 || std::memcmp(bytes_, "PPC10B01", 8) != 0)
            throw std::runtime_error("bad packed Portal closure");
        const uint16_t version = u16(bytes_ + 8);
        dimension_ = u16(bytes_ + 10);
        band_ = u16(bytes_ + 12);
        periods_ = u16(bytes_ + 14);
        direction_ = static_cast<int8_t>(bytes_[16]);
        if (version != 1 || dimension_ == 0 || periods_ == 0 ||
            (direction_ != 1 && direction_ != -1))
            throw std::runtime_error("unsupported packed Portal closure");
        packed_bytes_ =
            (static_cast<std::size_t>(dimension_) * dimension_ * 10 + 7) / 8 + 2;
        period_bytes_ = static_cast<std::size_t>(dimension_) * 4 + packed_bytes_;
        if (size_ != 20 + period_bytes_ * periods_)
            throw std::runtime_error("packed Portal closure size mismatch");
    }

    uint16_t lookup(uint16_t period, uint16_t source, uint16_t target) const {
        if (period == 0 || period > periods_ || source >= dimension_ || target >= dimension_)
            throw std::out_of_range("Portal lookup index outside table");
        const unsigned char* base =
            bytes_ + 20 + static_cast<std::size_t>(period - 1) * period_bytes_;
        const uint16_t row = u16(base + static_cast<std::size_t>(source) * 2);
        const uint16_t column =
            u16(base + static_cast<std::size_t>(dimension_ + target) * 2);
        const unsigned char* packed = base + static_cast<std::size_t>(dimension_) * 4;
        const std::size_t bit =
            (static_cast<std::size_t>(source) * dimension_ + target) * 10;
        const std::size_t byte = bit >> 3;
        const int shift = static_cast<int>(bit & 7);
        const uint32_t window = static_cast<uint32_t>(packed[byte]) |
            (static_cast<uint32_t>(packed[byte + 1]) << 8) |
            (static_cast<uint32_t>(packed[byte + 2]) << 16);
        return static_cast<uint16_t>(row + column + ((window >> shift) & 1023u));
    }

    uint32_t combine(uint16_t source_local, uint16_t period,
                     uint16_t source_event, uint16_t target_event,
                     uint16_t target_local) const {
        return static_cast<uint32_t>(source_local) +
               lookup(period, source_event, target_event) + target_local;
    }

    uint16_t dimension() const { return dimension_; }
    uint16_t band() const { return band_; }
    uint16_t periods() const { return periods_; }
    int direction() const { return direction_; }

private:
    static uint16_t u16(const unsigned char* value) {
        uint16_t result;
        std::memcpy(&result, value, sizeof(result));
        return result;
    }

    const unsigned char* bytes_ = nullptr;
    std::size_t size_ = 0;
    uint16_t dimension_ = 0;
    uint16_t band_ = 0;
    uint16_t periods_ = 0;
    int8_t direction_ = 0;
    std::size_t packed_bytes_ = 0;
    std::size_t period_bytes_ = 0;
};

} // namespace p10
