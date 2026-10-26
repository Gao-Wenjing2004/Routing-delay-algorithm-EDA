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
    if (argc != 2)
      throw std::runtime_error("usage: audit_route_periodicity output.json");

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
    const auto started = std::chrono::steady_clock::now();
    std::vector<Distance> dist(size_t(kWidth) * kWidth * kRoutes, DIST_INF);
    WorkHeap heap;

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

      for (int sx : {-1, 1})
        for (int sy : {-1, 1})
          for (int rx = 0; rx < kXPeriod; ++rx)
            for (int ry = 0; ry < kYPeriod; ++ry) {
              const int x = sx * (kXAnchor0 + rx);
              const int y = sy * (kYAnchor0 + ry);
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
                separable.add(v, vx + vy - vxy);
                // Predict the current cell exclusively from the lower 10x12
                // corner and its two preceding period slopes.
                affine_xy.add(v, vxy + (vxy - vxxy) + (vxy - vxyy));
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
