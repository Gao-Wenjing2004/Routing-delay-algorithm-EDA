// Audit lossless storage layouts for the exact radius-24 route-state table.
//
// The endpoint Base Atlas stores hundreds of millions of endpoint-pair
// values.  Those values are a min-plus composition of only 160 routing
// states.  This audit measures whether the exact route-state core can be
// stored directly as one row base plus packed residuals.  Unreachable states
// use the all-ones residual and therefore do not consume sparse exceptions.

#define SRB_DIST16
#define SRB_BASE_MARGIN 8
#include "../source/runtime_atlas_builder.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace srb_local_arch;
using namespace srb_runtime_atlas;

struct PackingMetric {
  unsigned bits = 0;
  uint64_t exceptions = 0;
};

uint64_t ceil_bits(uint64_t values, unsigned bits) {
  return (values * bits + 7) / 8;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2 && argc != 3)
      throw std::runtime_error(
          "usage: audit_exact_local_route_packing output.json [raw_u16.bin]");

    constexpr int kRadius = 24;
    constexpr int kMargin = SRB_BASE_MARGIN;
    constexpr int kWidth = 2 * (kRadius + kMargin) + 1;
    constexpr int kCenter = kRadius + kMargin;
    constexpr uint64_t kCells = uint64_t(2 * kRadius + 1) *
                                uint64_t(2 * kRadius + 1);
    constexpr uint64_t kRows = uint64_t(ROUTES) * kCells;
    constexpr uint64_t kValues = kRows * ROUTES;
    constexpr std::array<unsigned, 5> kBits{{7, 8, 9, 10, 11}};

    std::array<PackingMetric, kBits.size()> metrics{};
    for (size_t i = 0; i < metrics.size(); ++i) metrics[i].bits = kBits[i];

    uint64_t reachable = 0;
    uint64_t unreachable = 0;
    uint32_t maximum_value = 0;
    uint32_t maximum_row_minimum = 0;
    uint32_t maximum_residual = 0;
    uint64_t absolute_12bit_exceptions = 0;
    uint64_t absolute_13bit_exceptions = 0;

    std::ofstream raw;
    if (argc == 3) {
      raw.open(argv[2], std::ios::binary | std::ios::trunc);
      if (!raw) throw std::runtime_error("cannot create raw route table");
    }

    std::vector<Distance> dist(size_t(kWidth) * kWidth * ROUTES, DIST_INF);
    std::vector<uint16_t> source_values(size_t(kCells) * ROUTES, INF16);
    WorkHeap heap;
    const auto started = std::chrono::steady_clock::now();

    for (int source = 0; source < ROUTES; ++source) {
      std::fill(dist.begin(), dist.end(), DIST_INF);
      heap.clear();
      const uint32_t seed = state_id(kCenter, kCenter, source, kWidth);
      dist[seed] = 0;
      heap.push(0, seed);
      run_dijkstra(dist, heap, kWidth);

      size_t output_index = 0;
      for (int dy = -kRadius; dy <= kRadius; ++dy) {
        for (int dx = -kRadius; dx <= kRadius; ++dx) {
          uint32_t row_minimum = std::numeric_limits<uint32_t>::max();
          for (int target = 0; target < ROUTES; ++target) {
            const Distance value =
                dist[state_id(kCenter + dx, kCenter + dy, target, kWidth)];
            source_values[output_index + target] =
                value == DIST_INF ? INF16 : uint16_t(value);
            if (value != DIST_INF) row_minimum = std::min<uint32_t>(row_minimum, value);
          }
          if (row_minimum == std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("route row has no reachable target state");
          maximum_row_minimum = std::max(maximum_row_minimum, row_minimum);

          for (int target = 0; target < ROUTES; ++target) {
            const uint16_t value = source_values[output_index + target];
            if (value == INF16) {
              ++unreachable;
              continue;
            }
            ++reachable;
            maximum_value = std::max<uint32_t>(maximum_value, value);
            const uint32_t residual = uint32_t(value) - row_minimum;
            maximum_residual = std::max(maximum_residual, residual);
            for (PackingMetric& metric : metrics) {
              const uint32_t sentinel = (uint32_t(1) << metric.bits) - 1;
              metric.exceptions += residual >= sentinel;
            }
            absolute_12bit_exceptions += value >= 4095;
            absolute_13bit_exceptions += value >= 8191;
          }
          output_index += ROUTES;
        }
      }

      if (raw) {
        raw.write(reinterpret_cast<const char*>(source_values.data()),
                  std::streamsize(source_values.size() * sizeof(uint16_t)));
        if (!raw) throw std::runtime_error("raw route table write failed");
      }

      const double seconds = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
      std::fprintf(stderr, "\rlocal-route source=%d/%d elapsed=%.1fs",
                   source + 1, ROUTES, seconds);
      std::fflush(stderr);
    }
    std::fprintf(stderr, "\n");
    if (raw) raw.close();

    const double seconds = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - started)
                               .count();
    std::ofstream out(argv[1]);
    if (!out) throw std::runtime_error("cannot create report");
    out.precision(12);
    out << "{\n"
        << "  \"schema\": 1,\n"
        << "  \"radius\": " << kRadius << ",\n"
        << "  \"margin\": " << kMargin << ",\n"
        << "  \"routes\": " << ROUTES << ",\n"
        << "  \"rows\": " << kRows << ",\n"
        << "  \"values\": " << kValues << ",\n"
        << "  \"reachable\": " << reachable << ",\n"
        << "  \"unreachable\": " << unreachable << ",\n"
        << "  \"maximum_value\": " << maximum_value << ",\n"
        << "  \"maximum_row_minimum\": " << maximum_row_minimum << ",\n"
        << "  \"maximum_residual\": " << maximum_residual << ",\n"
        << "  \"raw_u16_bytes\": " << kValues * 2 << ",\n"
        << "  \"row_residual_layouts\": [\n";
    for (size_t i = 0; i < metrics.size(); ++i) {
      const PackingMetric& metric = metrics[i];
      const uint64_t payload = ceil_bits(kValues, metric.bits);
      const uint64_t bytes = kRows * 2 + payload + metric.exceptions * 6;
      out << "    {\"bits\": " << metric.bits
          << ", \"exceptions\": " << metric.exceptions
          << ", \"base_bytes\": " << kRows * 2
          << ", \"payload_bytes\": " << payload
          << ", \"exception_bytes_u32_u16\": " << metric.exceptions * 6
          << ", \"total_bytes\": " << bytes << "}"
          << (i + 1 == metrics.size() ? "" : ",") << "\n";
    }
    out << "  ],\n"
        << "  \"absolute_layouts\": [\n"
        << "    {\"bits\": 12, \"exceptions\": "
        << absolute_12bit_exceptions << ", \"total_bytes\": "
        << ceil_bits(kValues, 12) + absolute_12bit_exceptions * 6 << "},\n"
        << "    {\"bits\": 13, \"exceptions\": "
        << absolute_13bit_exceptions << ", \"total_bytes\": "
        << ceil_bits(kValues, 13) + absolute_13bit_exceptions * 6 << "}\n"
        << "  ],\n"
        << "  \"elapsed_seconds\": " << seconds << "\n"
        << "}\n";
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return 1;
  }
}
