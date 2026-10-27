#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace srb_p10 {

// Read-only view of the P10 row-base + column-base + 10-bit residual format.
// The table is generated offline from exact full-graph Dijkstra matrices.
class PackedPortalClosureView {
 public:
  PackedPortalClosureView() = default;
  PackedPortalClosureView(const unsigned char* bytes, std::size_t size) {
    reset(bytes, size);
  }

  void reset(const unsigned char* bytes, std::size_t size) {
    bytes_ = bytes;
    size_ = size;
    if (!bytes_ || size_ < 20 || std::memcmp(bytes_, "PPC10B01", 8))
      throw std::runtime_error("bad packed P10 Portal closure");
    const uint16_t version = u16(bytes_ + 8);
    dimension_ = u16(bytes_ + 10);
    band_ = u16(bytes_ + 12);
    periods_ = u16(bytes_ + 14);
    direction_ = static_cast<int8_t>(bytes_[16]);
    if (version != 1 || dimension_ != 1920 || band_ != 6 || periods_ != 5 ||
        (direction_ != 1 && direction_ != -1))
      throw std::runtime_error("unsupported packed P10 Portal closure");
    packed_bytes_ =
        (static_cast<std::size_t>(dimension_) * dimension_ * 10 + 7) / 8 + 2;
    period_bytes_ = static_cast<std::size_t>(dimension_) * 4 + packed_bytes_;
    if (size_ != 20 + period_bytes_ * periods_)
      throw std::runtime_error("packed P10 Portal closure size mismatch");
  }

  uint16_t lookup(unsigned period, unsigned source, unsigned target) const {
    if (!bytes_ || period < 1 || period > periods_ || source >= dimension_ ||
        target >= dimension_)
      throw std::out_of_range("P10 Portal lookup index outside table");
    return lookup_unchecked(period, source, target);
  }

  uint16_t lookup_unchecked(unsigned period, unsigned source,
                            unsigned target) const {
    const unsigned char* base =
        bytes_ + 20 + std::size_t(period - 1) * period_bytes_;
    const uint16_t row = u16(base + std::size_t(source) * 2);
    const uint16_t column =
        u16(base + std::size_t(dimension_ + target) * 2);
    const unsigned char* packed =
        base + static_cast<std::size_t>(dimension_) * 4;
    const std::size_t bit =
        (std::size_t(source) * dimension_ + target) * 10;
    const std::size_t byte = bit >> 3;
    const unsigned shift = unsigned(bit & 7);
    const uint32_t window = uint32_t(packed[byte]) |
                            (uint32_t(packed[byte + 1]) << 8) |
                            (uint32_t(packed[byte + 2]) << 16);
    return uint16_t(row + column + ((window >> shift) & 1023u));
  }

  uint16_t dimension() const { return dimension_; }
  uint16_t band() const { return band_; }
  uint16_t periods() const { return periods_; }
  int direction() const { return direction_; }
  std::size_t size_bytes() const { return size_; }

 private:
  static uint16_t u16(const unsigned char* value) {
    uint16_t result = 0;
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

}  // namespace srb_p10
