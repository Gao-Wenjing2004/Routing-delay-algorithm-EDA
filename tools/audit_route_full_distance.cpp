// Validate the compact 10x12 route-state affine table over the full chip
// displacement range (|x| <= 119, |y| <= 549).

#define SRB_DIST16
#include "../source/runtime_atlas_builder.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
using namespace srb_local_arch;
using namespace srb_runtime_atlas;

uint32_t rect_state(int x, int y, int route, int width) {
  return uint32_t((y * width + x) * ROUTES + route);
}

void run_rect(std::vector<Distance>& dist, WorkHeap& heap, int width,
              int height) {
  while (!heap.empty()) {
    const auto q = heap.pop();
    if (dist[q.second] != q.first) continue;
    const int route = q.second % ROUTES;
    const int cell = q.second / ROUTES;
    const int x = cell % width;
    const int y = cell / width;
    const uint16_t input = kRouteToInput[route];
    for (uint32_t i = kTransitionOffset[input]; i < kTransitionOffset[input + 1];
         ++i) {
      const auto& edge = kTransitions[i];
      const int nx = x + edge.dx;
      const int ny = y + edge.dy;
      if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
      const uint32_t next = rect_state(nx, ny, edge.next_route, width);
      const uint32_t candidate = q.first + edge.cost;
      if (candidate < dist[next]) {
        dist[next] = Distance(candidate);
        heap.push(candidate, next);
      }
    }
  }
}

int positive_mod(int value, int divisor) {
  const int result = value % divisor;
  return result < 0 ? result + divisor : result;
}

struct Metric {
  uint64_t count = 0;
  uint64_t exact = 0;
  uint64_t abs_error = 0;
  uint32_t max_error = 0;
  uint64_t score_samples = 0;
  long double score_sum = 0;

  void add(int golden, int predicted, bool score) {
    const uint32_t error = uint32_t(std::abs(golden - predicted));
    ++count;
    exact += error == 0;
    abs_error += error;
    if (error > max_error) max_error = error;
    if (score) {
      ++score_samples;
      score_sum += golden == 0
                       ? (predicted == 0 ? 1.0L : 0.0L)
                       : 1.0L - std::tanh(4.0L * error / golden);
    }
  }
};

void metric_json(std::ofstream& out, const char* name, const Metric& metric,
                 bool comma) {
  out << "    \"" << name << "\": {\n"
      << "      \"count\": " << metric.count << ",\n"
      << "      \"exact_rate\": "
      << (metric.count ? double(metric.exact) / metric.count : 0.0) << ",\n"
      << "      \"mae_ps\": "
      << (metric.count ? double(metric.abs_error) / metric.count : 0.0)
      << ",\n"
      << "      \"max_error_ps\": " << metric.max_error << ",\n"
      << "      \"score_samples\": " << metric.score_samples << ",\n"
      << "      \"contest_accuracy_sample\": "
      << (metric.score_samples
              ? 100.0 * double(metric.score_sum / metric.score_samples)
              : 0.0)
      << "\n    }" << (comma ? "," : "") << "\n";
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("usage: audit_route_full_distance output.json");
    constexpr int kMaxX = 119, kMaxY = 549, kMargin = 48;
    constexpr int kWidth = 2 * (kMaxX + kMargin) + 1;
    constexpr int kHeight = 2 * (kMaxY + kMargin) + 1;
    constexpr int kCx = kMaxX + kMargin;
    constexpr int kCy = kMaxY + kMargin;
    constexpr std::array<int, 6> kPeriods{{1, 2, 4, 8, 16, 32}};

    std::set<int> x_values;
    const std::array<int, 5> starts{{0, 24, 64, 95, 110}};
    for (const int start : starts)
      for (int residue = 0; residue < 10; ++residue) {
        const int value = start + residue;
        if (value <= kMaxX) {
          x_values.insert(value);
          if (value) x_values.insert(-value);
        }
      }

    Metric all;
    std::array<Metric, 5> x_bands;
    std::array<Metric, kPeriods.size()> y_spans;
    uint64_t unreachable = 0;
    const auto started = std::chrono::steady_clock::now();
    std::vector<Distance> dist(size_t(kWidth) * kHeight * ROUTES, DIST_INF);
    WorkHeap heap;

    auto at = [&](int x, int y, int route) -> int {
      const Distance value =
          dist[rect_state(kCx + x, kCy + y, route, kWidth)];
      return value == DIST_INF ? -1 : int(value);
    };

    for (int source = 0; source < ROUTES; ++source) {
      std::fill(dist.begin(), dist.end(), DIST_INF);
      heap.clear();
      const uint32_t seed = rect_state(kCx, kCy, source, kWidth);
      dist[seed] = 0;
      heap.push(0, seed);
      run_rect(dist, heap, kWidth, kHeight);

      for (const int x : x_values) {
        const int ax_abs = 95 + positive_mod(std::abs(x) - 95, 10);
        const int sx = x < 0 ? -1 : 1;
        const int ax = sx * ax_abs;
        const int nx = (std::abs(x) - ax_abs) / 10;
        const int ax0 = ax - sx * 10;
        const int abs_x = std::abs(x);
        const size_t band = abs_x <= 16 ? 0 : abs_x <= 32 ? 1
                                        : abs_x <= 72 ? 2
                                        : abs_x <= 104 ? 3
                                                      : 4;
        for (const int sy : {-1, 1})
          for (int residue = 0; residue < 12; ++residue) {
            const int ay_abs = 93 + residue;
            const int ay = sy * ay_abs;
            const int ay0 = ay - sy * 12;
            for (size_t span_index = 0; span_index < kPeriods.size();
                 ++span_index) {
              const int ny = kPeriods[span_index];
              const int y = sy * (ay_abs + ny * 12);
              if (std::abs(y) > kMaxY) continue;
              for (int target = 0; target < ROUTES; ++target) {
                const int golden = at(x, y, target);
                const int anchor = at(ax, ay, target);
                const int x_old = at(ax0, ay, target);
                const int y_old = at(ax, ay0, target);
                if (golden < 0 || anchor < 0 || x_old < 0 || y_old < 0) {
                  ++unreachable;
                  continue;
                }
                const int predicted = anchor + nx * (anchor - x_old) +
                                      ny * (anchor - y_old);
                const bool score = ((source * ROUTES + target) & 15) == 0;
                all.add(golden, predicted, score);
                x_bands[band].add(golden, predicted, score);
                y_spans[span_index].add(golden, predicted, score);
              }
            }
          }
      }
      const double elapsed = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
      std::fprintf(stderr, "\rfull-distance source=%d/%d elapsed=%.1fs",
                   source + 1, ROUTES, elapsed);
      std::fflush(stderr);
    }
    std::fprintf(stderr, "\n");

    std::ofstream out(argv[1]);
    if (!out) throw std::runtime_error("cannot create report");
    out.precision(12);
    out << "{\n"
        << "  \"schema\": 1,\n"
        << "  \"routes\": " << ROUTES << ",\n"
        << "  \"field_width\": " << kWidth << ",\n"
        << "  \"field_height\": " << kHeight << ",\n"
        << "  \"sampled_x_values\": " << x_values.size() << ",\n"
        << "  \"unreachable\": " << unreachable << ",\n"
        << "  \"elapsed_seconds\": "
        << std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         started)
               .count()
        << ",\n  \"metrics\": {\n";
    metric_json(out, "all", all, true);
    static constexpr const char* kBandNames[5] = {"x_0_16", "x_17_32",
                                                  "x_33_72", "x_73_104",
                                                  "x_105_119"};
    for (size_t i = 0; i < x_bands.size(); ++i)
      metric_json(out, kBandNames[i], x_bands[i], true);
    for (size_t i = 0; i < y_spans.size(); ++i) {
      const std::string name = "y_periods_" + std::to_string(kPeriods[i]);
      metric_json(out, name.c_str(), y_spans[i], i + 1 != y_spans.size());
    }
    out << "  }\n}\n";
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return 1;
  }
}
