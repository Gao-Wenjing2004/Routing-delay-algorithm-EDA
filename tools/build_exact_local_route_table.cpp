// Build the lossless radius-24 route-state table used by the O(1) local
// connector branch.  Each route stores the base 49x49 square plus only the
// strip needed by its compulsory Output Net displacement.

#define SRB_DIST16
#include "../source/runtime_atlas_builder.hpp"
#include "../source/exact_local_route_table.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace srb_local_arch;
using namespace srb_runtime_atlas;
using srb_exact_local::Header;
using srb_exact_local::RouteMetadata;
using srb_exact_local::ValueException;

constexpr int kRadius = 24;
constexpr int kMaxSeed = 12;
constexpr int kMargin = 8;
constexpr int kExtent = kRadius + kMaxSeed + kMargin;
constexpr int kWidth = 2 * kExtent + 1;
constexpr int kCenter = kExtent;
constexpr int kRowBytes = 182;

void pack_row(const std::array<uint16_t, ROUTES>& values, uint32_t row,
              std::array<unsigned char, kRowBytes>& packed,
              std::vector<ValueException>& exceptions) {
  uint16_t minimum = INF16;
  for (const uint16_t value : values)
    if (value != INF16) minimum = std::min(minimum, value);
  if (minimum == INF16) throw std::runtime_error("empty exact local row");
  packed.fill(0);
  std::memcpy(packed.data(), &minimum, sizeof(minimum));
  for (unsigned target = 0; target < ROUTES; ++target) {
    uint32_t residual =
        values[target] == INF16 ? 511u : uint32_t(values[target] - minimum);
    if (residual >= 511u) {
      residual = 511u;
      if (values[target] != INF16)
        exceptions.push_back({row * ROUTES + target, values[target]});
    }
    const unsigned bit = target * 9u;
    const unsigned byte = 2u + (bit >> 3);
    const unsigned shift = bit & 7u;
    const uint32_t word = residual << shift;
    packed[byte] |= uint8_t(word);
    packed[byte + 1] |= uint8_t(word >> 8);
  }
}

template <class Function>
void each_cell(int seed_dx, int seed_dy, Function&& function) {
  for (int dy = -kRadius; dy <= kRadius; ++dy)
    for (int dx = -kRadius; dx <= kRadius; ++dx) function(dx, dy);
  if (seed_dx > 0)
    for (int dy = -kRadius; dy <= kRadius; ++dy)
      for (int dx = -kRadius - seed_dx; dx < -kRadius; ++dx)
        function(dx, dy);
  else if (seed_dx < 0)
    for (int dy = -kRadius; dy <= kRadius; ++dy)
      for (int dx = kRadius + 1; dx <= kRadius - seed_dx; ++dx)
        function(dx, dy);
  else if (seed_dy > 0)
    for (int dx = -kRadius; dx <= kRadius; ++dx)
      for (int dy = -kRadius - seed_dy; dy < -kRadius; ++dy)
        function(dx, dy);
  else if (seed_dy < 0)
    for (int dx = -kRadius; dx <= kRadius; ++dx)
      for (int dy = kRadius + 1; dy <= kRadius - seed_dy; ++dy)
        function(dx, dy);
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("usage: build_exact_local_route_table output.bin");

    std::array<int8_t, ROUTES> seed_dx{};
    std::array<int8_t, ROUTES> seed_dy{};
    std::array<uint8_t, ROUTES> seen{};
    for (int port = 0; port < kPortCount; ++port) {
      if (kPortToInput[port] >= 0) continue;
      const auto& edge = kOutputNet[port];
      if (edge.next_route < 0) continue;
      const int route = edge.next_route;
      if (seen[route] &&
          (seed_dx[route] != edge.dx || seed_dy[route] != edge.dy))
        throw std::runtime_error("route has inconsistent Output Net seeds");
      seen[route] = 1;
      seed_dx[route] = edge.dx;
      seed_dy[route] = edge.dy;
    }
    for (int route = 0; route < ROUTES; ++route)
      if (!seen[route] || (seed_dx[route] && seed_dy[route]) ||
          std::max(std::abs(int(seed_dx[route])), std::abs(int(seed_dy[route]))) >
              kMaxSeed)
        throw std::runtime_error("missing or invalid route Output Net seed");
    for (int input = 0; input < kInputCount; ++input)
      if (kInputToRoute[input] < 0)
        for (uint32_t i = kTransitionOffset[input];
             i < kTransitionOffset[input + 1]; ++i) {
          const auto& edge = kTransitions[i];
          if (edge.dx != seed_dx[edge.next_route] ||
              edge.dy != seed_dy[edge.next_route])
            throw std::runtime_error("indirect Input seed is not a natural route seed");
        }

    std::array<RouteMetadata, ROUTES + 1> metadata{};
    uint32_t rows = 0;
    for (int route = 0; route < ROUTES; ++route) {
      metadata[route] = {rows, seed_dx[route], seed_dy[route], 0};
      rows += uint32_t((2 * kRadius + 1) * (2 * kRadius + 1) +
                       (2 * kRadius + 1) *
                           (std::abs(int(seed_dx[route])) +
                            std::abs(int(seed_dy[route]))));
    }
    metadata[ROUTES] = {rows, 0, 0, 0};

    Header header{};
    std::memcpy(header.magic, "SRBLRT1", 7);
    header.version = 1;
    header.routes = ROUTES;
    header.radius = kRadius;
    header.row_bytes = kRowBytes;
    header.rows = rows;
    header.values = rows * ROUTES;
    header.metadata_offset = sizeof(Header);
    header.data_offset = 4096;
    header.exception_offset = header.data_offset + uint64_t(rows) * kRowBytes;

    std::fstream out(argv[1], std::ios::binary | std::ios::in |
                                  std::ios::out | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create exact local route table");
    std::array<unsigned char, 4096> prefix{};
    std::memcpy(prefix.data(), &header, sizeof(header));
    std::memcpy(prefix.data() + header.metadata_offset, metadata.data(),
                metadata.size() * sizeof(RouteMetadata));
    out.write(reinterpret_cast<const char*>(prefix.data()), prefix.size());

    std::vector<Distance> dist(size_t(kWidth) * kWidth * ROUTES, DIST_INF);
    WorkHeap heap;
    std::array<uint16_t, ROUTES> values{};
    std::array<unsigned char, kRowBytes> packed{};
    std::vector<ValueException> exceptions;
    exceptions.reserve(65536);
    uint32_t row = 0;
    const auto started = std::chrono::steady_clock::now();
    for (int source = 0; source < ROUTES; ++source) {
      std::fill(dist.begin(), dist.end(), DIST_INF);
      heap.clear();
      const uint32_t start = state_id(kCenter, kCenter, source, kWidth);
      dist[start] = 0;
      heap.push(0, start);
      run_dijkstra(dist, heap, kWidth);
      each_cell(seed_dx[source], seed_dy[source], [&](int dx, int dy) {
        const uint32_t base = state_id(kCenter + dx, kCenter + dy, 0, kWidth);
        for (int target = 0; target < ROUTES; ++target)
          values[target] = dist[base + target];
        pack_row(values, row++, packed, exceptions);
        out.write(reinterpret_cast<const char*>(packed.data()), packed.size());
      });
      if (!out) throw std::runtime_error("exact local row write failed");
      const double seconds = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
      std::fprintf(stderr, "\rexact-local source=%d/%d rows=%u elapsed=%.1fs",
                   source + 1, ROUTES, row, seconds);
      std::fflush(stderr);
    }
    std::fprintf(stderr, "\n");
    if (row != rows) throw std::runtime_error("exact local row count mismatch");
    out.write(reinterpret_cast<const char*>(exceptions.data()),
              std::streamsize(exceptions.size() * sizeof(ValueException)));
    header.exceptions = uint32_t(exceptions.size());
    header.file_bytes = header.exception_offset +
                        uint64_t(header.exceptions) * sizeof(ValueException);
    out.seekp(0);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.close();
    if (!out) throw std::runtime_error("cannot finalize exact local route table");
    std::fprintf(stderr,
                 "rows=%u exceptions=%u bytes=%llu elapsed=%.3fs\n", rows,
                 header.exceptions,
                 static_cast<unsigned long long>(header.file_bytes),
                 std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                started)
                     .count());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return 1;
  }
}
