// Compare the packed table plus endpoint connectors against independent
// endpoint-seeded Dijkstra fields over a deterministic 1/64 Base-Atlas sample.

#define SRB_DIST16
#define SRB_BASE_MARGIN 8
#include "../source/runtime_atlas_builder.hpp"
#include "../source/exact_local_route_table.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

int main(int argc, char** argv) {
  try {
    if (argc != 3)
      throw std::runtime_error(
          "usage: validate_exact_local_route_table table.bin report.json");
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) throw std::runtime_error("cannot open exact local table");
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),
                                     std::istreambuf_iterator<char>());
    srb_exact_local::View table(bytes.data(), bytes.size());

    using namespace srb_local_arch;
    using namespace srb_runtime_atlas;
    constexpr int radius = 24, margin = 8;
    constexpr int width = 2 * (radius + margin) + 1;
    constexpr int center = radius + margin;
    std::vector<Distance> dist(size_t(width) * width * ROUTES, DIST_INF);
    WorkHeap heap;
    uint64_t sampled = 0, exact = 0, under = 0, over = 0;
    uint64_t absolute_error = 0;
    uint32_t max_error = 0;
    const auto started = std::chrono::steady_clock::now();

    for (size_t si = 0; si < std::size(srb_runtime_ids::base_sources); ++si) {
      const uint16_t source = srb_runtime_ids::base_sources[si];
      std::fill(dist.begin(), dist.end(), DIST_INF);
      heap.clear();
      seed_port(source, center, width, dist, heap);
      run_dijkstra(dist, heap, width);
      uint32_t cell = 0;
      for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx, ++cell)
          for (size_t ti = 0; ti < std::size(srb_runtime_ids::base_targets);
               ++ti) {
            if (((si * 131u + cell * 17u + ti * 7u) & 63u) != 0) continue;
            const uint16_t target = srb_runtime_ids::base_targets[ti];
            const uint16_t golden =
                target_value(dist, width, center, dx, dy, target);
            const uint16_t predicted = table.predict(source, target, dx, dy);
            ++sampled;
            if (golden == predicted) {
              ++exact;
              continue;
            }
            if (predicted < golden) ++under;
            else ++over;
            const uint32_t error = uint32_t(std::abs(int(golden) - int(predicted)));
            absolute_error += error;
            max_error = std::max(max_error, error);
          }
      const double elapsed = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
      std::fprintf(stderr, "\rvalidate source=%zu/%zu sample=%llu elapsed=%.1fs",
                   si + 1, std::size(srb_runtime_ids::base_sources),
                   static_cast<unsigned long long>(sampled), elapsed);
      std::fflush(stderr);
    }
    std::fprintf(stderr, "\n");

    std::ofstream report(argv[2]);
    if (!report) throw std::runtime_error("cannot create validation report");
    report.precision(12);
    report << "{\n"
           << "  \"schema\": 1,\n"
           << "  \"sampled\": " << sampled << ",\n"
           << "  \"exact\": " << exact << ",\n"
           << "  \"under\": " << under << ",\n"
           << "  \"over\": " << over << ",\n"
           << "  \"mae_ps\": "
           << (sampled ? double(absolute_error) / sampled : 0.0) << ",\n"
           << "  \"max_error_ps\": " << max_error << ",\n"
           << "  \"table_bytes\": " << bytes.size() << ",\n"
           << "  \"rows\": " << table.header().rows << ",\n"
           << "  \"exceptions\": " << table.header().exceptions << ",\n"
           << "  \"elapsed_seconds\": "
           << std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                            started)
                  .count()
           << "\n}\n";
    return exact == sampled ? 0 : 2;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return 1;
  }
}
