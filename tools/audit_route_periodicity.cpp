// Audit eventual 10x12 periodicity at the canonical 160 routing-state layer.
//
// This deliberately bypasses endpoint names, Blocks and the learned model.
// Each source route gets one exact, obstacle-free Dijkstra field.  We then
// test whether route-to-route distances near radius 104 have stable horizontal
// and vertical tropical slopes.  A positive result justifies an O(1) table of
// (anchor, x_slope, y_slope) indexed by route pair, quadrant and residue.

#define SRB_DIST16
#include "../source/runtime_atlas_builder.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace srb_runtime_atlas;

struct Metric {
  uint64_t comparable = 0;
  uint64_t exact = 0;
  uint64_t abs_error = 0;
  uint32_t max_error = 0;

  void add(int actual, int predicted) {
    const uint32_t error = uint32_t(std::abs(actual - predicted));
    ++comparable;
    exact += error == 0;
    abs_error += error;
    if (error > max_error) max_error = error;
  }
};

#pragma pack(push, 1)
struct TableHeader {
  char magic[8];
  uint32_t version;
  uint32_t routes;
  uint32_t x_period;
  uint32_t y_period;
  uint32_t x_anchor0;
  uint32_t y_anchor0;
  uint32_t quadrants;
  uint64_t records;
  uint64_t bytes;
  uint8_t reserved[12];
};

struct TableRecord {
  uint16_t anchor;
  int16_t x_slope;
  int16_t y_slope;
};
#pragma pack(pop)

static_assert(sizeof(TableHeader) == 64, "period table header layout");
static_assert(sizeof(TableRecord) == 6, "period table record layout");

struct LocalPacking {
  int radius = 0;
  uint64_t blocks = 0;
  uint64_t values = 0;
  uint64_t delta_u8_exceptions = 0;
};

void write_metric(std::ofstream& out, const char* name, const Metric& value,
                  bool comma = true) {
  const double exact = value.comparable
                           ? double(value.exact) / double(value.comparable)
                           : 0.0;
  const double mae = value.comparable
                         ? double(value.abs_error) / double(value.comparable)
                         : 0.0;
  out << "    \"" << name << "\": {\n"
      << "      \"comparable\": " << value.comparable << ",\n"
      << "      \"exact\": " << value.exact << ",\n"
      << "      \"exact_rate\": " << exact << ",\n"
      << "      \"mae_ps\": " << mae << ",\n"
      << "      \"max_error_ps\": " << value.max_error << "\n"
      << "    }" << (comma ? "," : "") << "\n";
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2 && argc != 3)
      throw std::runtime_error(
          "usage: audit_route_periodicity output.json [table.bin]");

    constexpr int kRoutes = 160;
    constexpr int kXPeriod = 10;
    constexpr int kYPeriod = 12;
    // Anchors cover every residue while all predecessor samples remain well
    // inside the field.  The 48-cell margin avoids boundary-truncated paths.
    constexpr int kXAnchor0 = 95;
    constexpr int kYAnchor0 = 93;
    constexpr int kRadius = 104;
    constexpr int kMargin = 48;
    constexpr int kCenter = kRadius + kMargin;
    constexpr int kWidth = 2 * kCenter + 1;

    Metric x_slope, y_slope, separable, affine_xy;
    uint64_t unreachable = 0;
    uint64_t packed_blocks = 0;
    uint64_t anchor_u8_exceptions = 0;
    uint64_t x_slope_i8_exceptions = 0;
    uint64_t y_slope_i8_exceptions = 0;
    std::array<LocalPacking, 5> local_packing{{
        {4, 0, 0, 0}, {8, 0, 0, 0}, {12, 0, 0, 0},
        {16, 0, 0, 0}, {24, 0, 0, 0}}};
    const auto started = std::chrono::steady_clock::now();
    std::vector<Distance> dist(size_t(kWidth) * kWidth * kRoutes, DIST_INF);
    WorkHeap heap;
    std::ofstream table;
    if (argc == 3) {
      table.open(argv[2], std::ios::binary | std::ios::trunc);
      if (!table) throw std::runtime_error("cannot create period table");
      TableHeader header{};
      std::memcpy(header.magic, "SRBPER1", 7);
      header.version = 1;
      header.routes = kRoutes;
      header.x_period = kXPeriod;
      header.y_period = kYPeriod;
      header.x_anchor0 = kXAnchor0;
      header.y_anchor0 = kYAnchor0;
      header.quadrants = 4;
      header.records = uint64_t(4) * kXPeriod * kYPeriod * kRoutes * kRoutes;
      header.bytes = header.records * sizeof(TableRecord);
      table.write(reinterpret_cast<const char*>(&header), sizeof(header));
    }

    auto value = [&](int x, int y, int route) -> int {
      const Distance result = dist[state_id(kCenter + x, kCenter + y, route,
                                            kWidth)];
      return result == DIST_INF ? -1 : int(result);
    };

    for (int source = 0; source < kRoutes; ++source) {
      std::fill(dist.begin(), dist.end(), DIST_INF);
      heap.clear();
      const uint32_t start = state_id(kCenter, kCenter, source, kWidth);
      dist[start] = 0;
      heap.push(0, start);
      run_dijkstra(dist, heap, kWidth);

      // Exact local route tables can share one uint16 minimum across all 160
      // target routes at a displacement and store uint8 deltas plus sparse
      // escapes.  Measure the real capacity instead of estimating it.
      for (auto& packing : local_packing) {
        const int radius = packing.radius;
        for (int dy = -radius; dy <= radius; ++dy)
          for (int dx = -radius; dx <= radius; ++dx) {
            int minimum = std::numeric_limits<int>::max();
            for (int target = 0; target < kRoutes; ++target) {
              const int current = value(dx, dy, target);
              if (current >= 0) minimum = std::min(minimum, current);
            }
            ++packing.blocks;
            packing.values += kRoutes;
            for (int target = 0; target < kRoutes; ++target) {
              const int current = value(dx, dy, target);
              packing.delta_u8_exceptions += current < 0 ||
                  unsigned(current - minimum) >= 255;
            }
          }
      }

      for (int sx : {-1, 1})
        for (int sy : {-1, 1})
          for (int rx = 0; rx < kXPeriod; ++rx)
            for (int ry = 0; ry < kYPeriod; ++ry) {
              const int x = sx * (kXAnchor0 + rx);
              const int y = sy * (kYAnchor0 + ry);
              std::array<int, kRoutes> anchors{};
              std::array<int, kRoutes> x_slopes{};
              std::array<int, kRoutes> y_slopes{};
              anchors.fill(65535);
              int minimum_anchor = std::numeric_limits<int>::max();
              for (int target = 0; target < kRoutes; ++target) {
                const int v = value(x, y, target);
                const int vx = value(x - sx * kXPeriod, y, target);
                const int vxx = value(x - sx * 2 * kXPeriod, y, target);
                const int vy = value(x, y - sy * kYPeriod, target);
                const int vyy = value(x, y - sy * 2 * kYPeriod, target);
                const int vxy = value(x - sx * kXPeriod,
                                      y - sy * kYPeriod, target);
                const int vxxy = value(x - sx * 2 * kXPeriod,
                                       y - sy * kYPeriod, target);
                const int vxyy = value(x - sx * kXPeriod,
                                       y - sy * 2 * kYPeriod, target);
                if (v < 0 || vx < 0 || vxx < 0 || vy < 0 || vyy < 0 ||
                    vxy < 0 || vxxy < 0 || vxyy < 0) {
                  ++unreachable;
                  continue;
                }
                x_slope.add(v - vx, vx - vxx);
                y_slope.add(v - vy, vy - vyy);
                anchors[target] = v;
                x_slopes[target] = v - vx;
                y_slopes[target] = v - vy;
                minimum_anchor = std::min(minimum_anchor, v);
                separable.add(v, vx + vy - vxy);
                // Predict the current cell exclusively from the lower 10x12
                // corner and its two preceding period slopes.
                affine_xy.add(v, vxy + (vxy - vxxy) + (vxy - vxyy));
              }
              if (minimum_anchor != std::numeric_limits<int>::max()) {
                ++packed_blocks;
                for (int target = 0; target < kRoutes; ++target) {
                  if (anchors[target] == 65535) {
                    ++anchor_u8_exceptions;
                    continue;
                  }
                  anchor_u8_exceptions +=
                      unsigned(anchors[target] - minimum_anchor) >= 255;
                  x_slope_i8_exceptions +=
                      x_slopes[target] < -127 || x_slopes[target] > 127;
                  y_slope_i8_exceptions +=
                      y_slopes[target] < -127 || y_slopes[target] > 127;
                }
              }
              if (table) {
                for (int target = 0; target < kRoutes; ++target) {
                  const TableRecord record{uint16_t(anchors[target]),
                                           int16_t(x_slopes[target]),
                                           int16_t(y_slopes[target])};
                  table.write(reinterpret_cast<const char*>(&record),
                              sizeof(record));
                }
                if (!table) throw std::runtime_error("period table write failed");
              }
            }
      const double seconds = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
      std::fprintf(stderr, "\rroute source=%d/%d elapsed=%.1fs", source + 1,
                   kRoutes, seconds);
      std::fflush(stderr);
    }
    std::fprintf(stderr, "\n");
    if (table) table.close();

    const double seconds = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - started)
                               .count();
    std::ofstream out(argv[1]);
    if (!out) throw std::runtime_error("cannot create report");
    out.precision(12);
    out << "{\n"
        << "  \"schema\": 1,\n"
        << "  \"routes\": " << kRoutes << ",\n"
        << "  \"x_period\": " << kXPeriod << ",\n"
        << "  \"y_period\": " << kYPeriod << ",\n"
        << "  \"anchor_x_range\": [" << kXAnchor0 << ", " << kRadius
        << "],\n"
        << "  \"anchor_y_range\": [" << kYAnchor0 << ", " << kRadius
        << "],\n"
        << "  \"quadrants\": 4,\n"
        << "  \"candidate_records\": "
        << uint64_t(4) * kXPeriod * kYPeriod * kRoutes * kRoutes << ",\n"
        << "  \"candidate_bytes_u16_i16_i16\": "
        << uint64_t(4) * kXPeriod * kYPeriod * kRoutes * kRoutes * 6 << ",\n"
        << "  \"packing\": {\n"
        << "    \"blocks\": " << packed_blocks << ",\n"
        << "    \"base_bytes_u8_anchor_i16_slopes\": "
        << packed_blocks * uint64_t(2 + kRoutes + 4 * kRoutes) << ",\n"
        << "    \"base_bytes_u8_anchor_i8_slopes\": "
        << packed_blocks * uint64_t(2 + 3 * kRoutes) << ",\n"
        << "    \"anchor_u8_exceptions\": " << anchor_u8_exceptions
        << ",\n"
        << "    \"x_slope_i8_exceptions\": " << x_slope_i8_exceptions
        << ",\n"
        << "    \"y_slope_i8_exceptions\": " << y_slope_i8_exceptions
        << ",\n"
        << "    \"exception_bytes_if_index32_value16\": "
        << uint64_t(6) * (anchor_u8_exceptions + x_slope_i8_exceptions +
                          y_slope_i8_exceptions)
        << "\n  },\n"
        << "  \"local_table_packing\": [\n";
    for (size_t i = 0; i < local_packing.size(); ++i) {
      const auto& packing = local_packing[i];
      const uint64_t packed = packing.blocks * uint64_t(2 + kRoutes) +
                              packing.delta_u8_exceptions * 6;
      out << "    {\"radius\": " << packing.radius
          << ", \"blocks\": " << packing.blocks
          << ", \"values\": " << packing.values
          << ", \"raw_u16_bytes\": " << packing.values * 2
          << ", \"delta_u8_exceptions\": "
          << packing.delta_u8_exceptions << ", \"packed_bytes\": " << packed
          << "}" << (i + 1 == local_packing.size() ? "" : ",") << "\n";
    }
    out << "  ],\n"
        << "  \"unreachable_combinations\": " << unreachable << ",\n"
        << "  \"elapsed_seconds\": " << seconds << ",\n"
        << "  \"metrics\": {\n";
    write_metric(out, "x_slope_stability", x_slope);
    write_metric(out, "y_slope_stability", y_slope);
    write_metric(out, "xy_separability", separable);
    write_metric(out, "affine_xy_prediction", affine_xy, false);
    out << "  }\n}\n";
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return 1;
  }
}
