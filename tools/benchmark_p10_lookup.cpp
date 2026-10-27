#include "../source/p10_periodic_portal.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>

extern "C" {
extern const unsigned char srb_p10_up_begin[];
extern const unsigned char srb_p10_up_end[];
extern const unsigned char srb_p10_down_begin[];
extern const unsigned char srb_p10_down_end[];
}

int main() {
  try {
    srb_p10::PackedPortalClosureView up(
        srb_p10_up_begin, size_t(srb_p10_up_end - srb_p10_up_begin));
    srb_p10::PackedPortalClosureView down(
        srb_p10_down_begin, size_t(srb_p10_down_end - srb_p10_down_begin));
    constexpr uint64_t iterations = 10000000;
    uint32_t state = 0x9e3779b9u;
    uint64_t checksum = 0;
    const auto started = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < iterations; ++i) {
      state = state * 1664525u + 1013904223u;
      const unsigned source = state % 1920u;
      state = state * 1664525u + 1013904223u;
      const unsigned target = state % 1920u;
      const unsigned period = unsigned(i % 5u) + 1u;
      checksum += (i & 1 ? down : up).lookup_unchecked(period, source, target);
    }
    const double seconds = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - started)
                               .count();
    std::printf(
        "{\n  \"lookups\": %llu,\n  \"seconds\": %.12f,\n  "
        "\"ns_per_lookup\": %.6f,\n  \"checksum\": %llu,\n  "
        "\"bytes\": %llu\n}\n",
        static_cast<unsigned long long>(iterations), seconds,
        seconds * 1e9 / iterations,
        static_cast<unsigned long long>(checksum),
        static_cast<unsigned long long>(up.size_bytes() + down.size_bytes()));
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return 1;
  }
}
