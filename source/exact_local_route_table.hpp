#pragma once

#include "local_arch_data.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace srb_exact_local {

#pragma pack(push, 1)
struct Header {
  char magic[8];
  uint32_t version;
  uint32_t routes;
  uint32_t radius;
  uint32_t row_bytes;
  uint32_t rows;
  uint32_t values;
  uint32_t exceptions;
  uint32_t metadata_offset;
  uint64_t data_offset;
  uint64_t exception_offset;
  uint64_t file_bytes;
};

struct RouteMetadata {
  uint32_t row_offset;
  int8_t seed_dx;
  int8_t seed_dy;
  uint16_t reserved;
};

struct ValueException {
  uint32_t index;
  uint16_t value;
};
#pragma pack(pop)

static_assert(sizeof(Header) == 64, "exact local table header layout");
static_assert(sizeof(RouteMetadata) == 8, "exact local route metadata layout");
static_assert(sizeof(ValueException) == 6, "exact local exception layout");

class View {
 public:
  View() = default;
  View(const unsigned char* bytes, size_t size) { reset(bytes, size); }

  void reset(const unsigned char* bytes, size_t size) {
    bytes_ = bytes;
    size_ = size;
    if (!bytes_ || size_ < sizeof(Header))
      throw std::runtime_error("truncated exact local route table");
    std::memcpy(&header_, bytes_, sizeof(header_));
    if (std::memcmp(header_.magic, "SRBLRT1", 7) || header_.version != 1 ||
        header_.routes != 160 || header_.radius != 24 ||
        header_.row_bytes != 182 || header_.values != header_.rows * 160u ||
        header_.metadata_offset < sizeof(Header) ||
        header_.data_offset < header_.metadata_offset + 161u * sizeof(RouteMetadata) ||
        header_.exception_offset !=
            header_.data_offset + uint64_t(header_.rows) * header_.row_bytes ||
        header_.file_bytes !=
            header_.exception_offset +
                uint64_t(header_.exceptions) * sizeof(ValueException) ||
        header_.file_bytes != size_)
      throw std::runtime_error("invalid exact local route table");
    metadata_ = reinterpret_cast<const RouteMetadata*>(
        bytes_ + header_.metadata_offset);
    data_ = bytes_ + header_.data_offset;
    exceptions_ = reinterpret_cast<const ValueException*>(
        bytes_ + header_.exception_offset);
    if (metadata_[0].row_offset != 0 ||
        metadata_[160].row_offset != header_.rows)
      throw std::runtime_error("bad exact local route offsets");
    for (int route = 0; route < 160; ++route) {
      if (metadata_[route].row_offset > metadata_[route + 1].row_offset ||
          metadata_[route].reserved ||
          (metadata_[route].seed_dx && metadata_[route].seed_dy))
        throw std::runtime_error("bad exact local route metadata");
    }
  }

  uint16_t lookup(int source_route, int dx, int dy, int target_route) const {
    if (!data_ || source_route < 0 || source_route >= 160 ||
        target_route < 0 || target_route >= 160)
      return kInf;
    const int cell = cell_index(source_route, dx, dy);
    if (cell < 0) return kInf;
    const uint32_t row = metadata_[source_route].row_offset + uint32_t(cell);
    if (row >= metadata_[source_route + 1].row_offset) return kInf;
    const unsigned char* record = data_ + uint64_t(row) * header_.row_bytes;
    uint16_t base = 0;
    std::memcpy(&base, record, sizeof(base));
    const unsigned bit = unsigned(target_route) * 9u;
    const unsigned byte = bit >> 3;
    const unsigned shift = bit & 7u;
    const uint32_t word = uint32_t(record[2 + byte]) |
                          (uint32_t(record[2 + byte + 1]) << 8);
    const uint16_t residual = uint16_t((word >> shift) & 511u);
    if (residual != 511u) return uint16_t(base + residual);
    const uint32_t value_index = row * 160u + uint32_t(target_route);
    const ValueException* first = exceptions_;
    const ValueException* last = exceptions_ + header_.exceptions;
    const ValueException* found = std::lower_bound(
        first, last, value_index,
        [](const ValueException& item, uint32_t index) {
          return item.index < index;
        });
    return found != last && found->index == value_index ? found->value : kInf;
  }

  uint16_t predict(uint16_t source_port, uint16_t target_port, int dx,
                   int dy) const {
    using namespace srb_local_arch;
    struct Seed {
      int route;
      int dx;
      int dy;
      int cost;
    };
    Seed seeds[32];
    int source_count = 0;
    const int source_input = kPortToInput[source_port];
    if (source_input >= 0) {
      const int route = kInputToRoute[source_input];
      if (route >= 0) {
        seeds[source_count++] = {route, 0, 0, 0};
      } else {
        for (uint32_t i = kTransitionOffset[source_input];
             i < kTransitionOffset[source_input + 1] && source_count < 32;
             ++i) {
          const auto& edge = kTransitions[i];
          seeds[source_count++] =
              {edge.next_route, edge.dx, edge.dy, edge.cost};
        }
      }
    } else {
      const auto& edge = kOutputNet[source_port];
      if (edge.next_route >= 0)
        seeds[source_count++] = {edge.next_route, edge.dx, edge.dy, 0};
    }

    srb_local_arch::PortCost targets[32];
    int target_count = 0;
    const int target_input = kPortToInput[target_port];
    if (target_input >= 0) {
      const int route = kInputToRoute[target_input];
      if (route >= 0) targets[target_count++] = {uint16_t(route), 0};
    } else {
      for (uint32_t i = kTargetOffset[target_port];
           i < kTargetOffset[target_port + 1] && target_count < 32; ++i)
        targets[target_count++] = kTargetArcs[i];
    }

    uint32_t best = std::numeric_limits<uint32_t>::max();
    for (int si = 0; si < source_count; ++si) {
      const Seed& seed = seeds[si];
      for (int ti = 0; ti < target_count; ++ti) {
        const uint16_t core = lookup(seed.route, dx - seed.dx, dy - seed.dy,
                                     targets[ti].id);
        if (core != kInf)
          best = std::min(best, uint32_t(core) + uint32_t(seed.cost) +
                                    uint32_t(targets[ti].cost));
      }
    }
    return best >= kInf ? kInf : uint16_t(best);
  }

  const Header& header() const { return header_; }
  size_t size_bytes() const { return size_; }

 private:
  static constexpr uint16_t kInf = 65535;

  int cell_index(int route, int dx, int dy) const {
    constexpr int radius = 24;
    constexpr int width = 49;
    if (dx >= -radius && dx <= radius && dy >= -radius && dy <= radius)
      return (dy + radius) * width + dx + radius;

    const int sx = metadata_[route].seed_dx;
    const int sy = metadata_[route].seed_dy;
    if (sx > 0 && dy >= -radius && dy <= radius && dx >= -radius - sx &&
        dx < -radius)
      return width * width + (dy + radius) * sx + dx + radius + sx;
    if (sx < 0 && dy >= -radius && dy <= radius && dx > radius &&
        dx <= radius - sx)
      return width * width + (dy + radius) * (-sx) + dx - radius - 1;
    if (sy > 0 && dx >= -radius && dx <= radius && dy >= -radius - sy &&
        dy < -radius)
      return width * width + (dx + radius) * sy + dy + radius + sy;
    if (sy < 0 && dx >= -radius && dx <= radius && dy > radius &&
        dy <= radius - sy)
      return width * width + (dx + radius) * (-sy) + dy - radius - 1;
    return -1;
  }

  const unsigned char* bytes_ = nullptr;
  size_t size_ = 0;
  Header header_{};
  const RouteMetadata* metadata_ = nullptr;
  const unsigned char* data_ = nullptr;
  const ValueException* exceptions_ = nullptr;
};

}  // namespace srb_exact_local
