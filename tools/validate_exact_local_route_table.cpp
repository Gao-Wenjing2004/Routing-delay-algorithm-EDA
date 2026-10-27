// Compare the packed table plus endpoint connectors against independent
// endpoint-seeded Dijkstra fields over a deterministic 1/64 Base-Atlas sample.

#define SRB_DIST16
#include "../source/runtime_atlas_builder.hpp"
#include "../source/exact_local_route_table.hpp"

#include <chrono>
#include <array>
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
    const int radius = int(table.radius());
    constexpr int maximum_seed = 12, margin = 32;
    const int extent = radius + maximum_seed + margin;
    const int width = 2 * extent + 1;
    const int center = extent;
    std::vector<Distance> dist(size_t(width) * width * ROUTES, DIST_INF);
    WorkHeap heap;
    uint64_t sampled = 0, exact = 0, under = 0, over = 0;
    uint64_t golden_inf_predicted_finite = 0;
    uint64_t golden_finite_predicted_inf = 0;
    uint64_t finite_value_mismatch = 0;
    std::array<uint64_t, 3> class_sampled{}, class_mismatch{};
    uint32_t printed_mismatches = 0;
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
            const int source_input = kPortToInput[source];
            const int source_class =
                source_input < 0 ? 1 : kInputToRoute[source_input] >= 0 ? 0 : 2;
            const uint16_t golden =
                target_value(dist, width, center, dx, dy, target);
            const uint16_t predicted = table.predict(source, target, dx, dy);
            ++sampled;
            ++class_sampled[source_class];
            if (golden == predicted) {
              ++exact;
              continue;
            }
            ++class_mismatch[source_class];
            if (golden == INF16 && predicted != INF16)
              ++golden_inf_predicted_finite;
            else if (golden != INF16 && predicted == INF16)
              ++golden_finite_predicted_inf;
            else
              ++finite_value_mismatch;
            if (printed_mismatches < 32) {
              std::fprintf(stderr,
                           "\nmismatch source=%u class=%d target=%u dx=%d dy=%d "
                           "golden=%u predicted=%u",
                           unsigned(source), source_class, unsigned(target), dx,
                           dy, unsigned(golden), unsigned(predicted));
              ++printed_mismatches;
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
           << "  \"radius\": " << radius << ",\n"
           << "  \"sampled\": " << sampled << ",\n"
           << "  \"exact\": " << exact << ",\n"
           << "  \"under\": " << under << ",\n"
           << "  \"over\": " << over << ",\n"
           << "  \"golden_inf_predicted_finite\": "
           << golden_inf_predicted_finite << ",\n"
           << "  \"golden_finite_predicted_inf\": "
           << golden_finite_predicted_inf << ",\n"
           << "  \"finite_value_mismatch\": " << finite_value_mismatch
           << ",\n"
           << "  \"source_classes\": {\n"
           << "    \"direct_input\": {\"sampled\": " << class_sampled[0]
           << ", \"mismatch\": " << class_mismatch[0] << "},\n"
           << "    \"output\": {\"sampled\": " << class_sampled[1]
           << ", \"mismatch\": " << class_mismatch[1] << "},\n"
           << "    \"indirect_input\": {\"sampled\": " << class_sampled[2]
           << ", \"mismatch\": " << class_mismatch[2] << "}\n"
           << "  },\n"
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
