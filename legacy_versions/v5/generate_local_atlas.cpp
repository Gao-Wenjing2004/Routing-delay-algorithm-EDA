#include "local_arch_data.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t INF32 = std::numeric_limits<uint32_t>::max();
constexpr uint16_t INF16 = std::numeric_limits<uint16_t>::max();
constexpr int ATLAS_RADIUS = 56;  // Query radius 48 plus one maximum-span source Net.
constexpr int SEARCH_MARGIN = 8;
constexpr int SEARCH_RADIUS = ATLAS_RADIUS + SEARCH_MARGIN;
constexpr int SEARCH_WIDTH = SEARCH_RADIUS * 2 + 1;
constexpr int ATLAS_WIDTH = ATLAS_RADIUS * 2 + 1;
constexpr int ROUTES = srb_local_arch::kRouteCount;
constexpr int SEARCH_CELLS = SEARCH_WIDTH * SEARCH_WIDTH;
constexpr int ATLAS_CELLS = ATLAS_WIDTH * ATLAS_WIDTH;

#pragma pack(push, 1)
struct AtlasHeader {
    char magic[8];
    uint32_t version;
    uint32_t radius;
    uint32_t routes;
    uint32_t cells;
    uint32_t sources;
    uint32_t header_bytes;
    uint64_t values;
};
#pragma pack(pop)

class RadixHeap {
public:
    using Item = std::pair<uint32_t, uint32_t>;  // key, state

    void clear() {
        for (auto& bucket : buckets_) bucket.clear();
        last_ = 0;
        size_ = 0;
    }

    bool empty() const { return size_ == 0; }

    void push(uint32_t key, uint32_t state) {
        if (key < last_) throw std::runtime_error("radix heap key is not monotone");
        buckets_[bucket_index(key, last_)].push_back({key, state});
        ++size_;
    }

    Item pop() {
        if (buckets_[0].empty()) pull();
        Item result = buckets_[0].back();
        buckets_[0].pop_back();
        --size_;
        return result;
    }

private:
    static int bucket_index(uint32_t key, uint32_t last) {
        uint32_t value = key ^ last;
        if (value == 0) return 0;
#if defined(__GNUC__)
        return 32 - __builtin_clz(value);
#else
        int bits = 0;
        while (value) { ++bits; value >>= 1; }
        return bits;
#endif
    }

    void pull() {
        int index = 1;
        while (index < 33 && buckets_[index].empty()) ++index;
        if (index == 33) throw std::runtime_error("pop from empty radix heap");
        uint32_t next_last = INF32;
        for (const Item& item : buckets_[index]) next_last = std::min(next_last, item.first);
        last_ = next_last;
        std::vector<Item> items;
        items.swap(buckets_[index]);
        for (const Item& item : items) {
            buckets_[bucket_index(item.first, last_)].push_back(item);
        }
    }

    std::array<std::vector<Item>, 33> buckets_;
    uint32_t last_ = 0;
    uint64_t size_ = 0;
};

inline uint32_t state_id(int x, int y, int route) {
    return static_cast<uint32_t>((y * SEARCH_WIDTH + x) * ROUTES + route);
}

void run_one_source(int source_route,
                    std::vector<uint32_t>& dist,
                    std::vector<uint16_t>& slab,
                    RadixHeap& heap) {
    using namespace srb_local_arch;
    std::fill(dist.begin(), dist.end(), INF32);
    heap.clear();
    const uint32_t start = state_id(SEARCH_RADIUS, SEARCH_RADIUS, source_route);
    dist[start] = 0;
    heap.push(0, start);
    uint64_t remaining = static_cast<uint64_t>(ATLAS_CELLS) * ROUTES;

    while (!heap.empty()) {
        const auto item = heap.pop();
        const uint32_t d = item.first;
        const uint32_t state = item.second;
        if (dist[state] != d) continue;

        const int route = static_cast<int>(state % ROUTES);
        const int cell = static_cast<int>(state / ROUTES);
        const int x = cell % SEARCH_WIDTH;
        const int y = cell / SEARCH_WIDTH;
        if (x >= SEARCH_MARGIN && x < SEARCH_MARGIN + ATLAS_WIDTH &&
            y >= SEARCH_MARGIN && y < SEARCH_MARGIN + ATLAS_WIDTH) {
            if (remaining > 0) --remaining;
            if (remaining == 0) break;
        }

        const uint16_t input = kRouteToInput[route];
        for (uint32_t ei = kTransitionOffset[input]; ei < kTransitionOffset[input + 1]; ++ei) {
            const Transition& edge = kTransitions[ei];
            const int nx = x + edge.dx;
            const int ny = y + edge.dy;
            if (nx < 0 || nx >= SEARCH_WIDTH || ny < 0 || ny >= SEARCH_WIDTH) continue;
            const uint32_t next = state_id(nx, ny, edge.next_route);
            const uint32_t nd = d + edge.cost;
            if (nd < dist[next]) {
                dist[next] = nd;
                heap.push(nd, next);
            }
        }
    }

    size_t out = 0;
    for (int dy = -ATLAS_RADIUS; dy <= ATLAS_RADIUS; ++dy) {
        const int y = SEARCH_RADIUS + dy;
        for (int dx = -ATLAS_RADIUS; dx <= ATLAS_RADIUS; ++dx) {
            const int x = SEARCH_RADIUS + dx;
            const size_t base = static_cast<size_t>(state_id(x, y, 0));
            for (int route = 0; route < ROUTES; ++route) {
                const uint32_t value = dist[base + route];
                slab[out++] = value >= INF16 ? INF16 : static_cast<uint16_t>(value);
            }
        }
    }
}

bool verify_existing_atlas(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return false;
    const std::streamoff file_size = input.tellg();
    input.seekg(0, std::ios::beg);
    AtlasHeader header{};
    input.read(reinterpret_cast<char*>(&header), sizeof(header));
    const uint64_t values = static_cast<uint64_t>(ROUTES) * ATLAS_CELLS * ROUTES;
    const uint64_t bytes = sizeof(AtlasHeader) + values * sizeof(uint16_t);
    return input && std::memcmp(header.magic, "SRBLAT2", 7) == 0 &&
           header.version == 1 && header.radius == ATLAS_RADIUS &&
           header.routes == ROUTES && header.cells == ATLAS_CELLS &&
           header.sources == ROUTES && header.header_bytes == sizeof(AtlasHeader) &&
           header.values == values && file_size >= 0 &&
           static_cast<uint64_t>(file_size) == bytes;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--check") {
            if (!verify_existing_atlas(argv[2])) {
                std::cerr << "Atlas is missing, truncated, or incompatible: " << argv[2] << '\n';
                return 1;
            }
            std::cout << "Atlas is compatible: " << argv[2] << '\n';
            return 0;
        }
        if (argc != 2) {
            std::cerr << "Usage:\n  " << argv[0] << " local_atlas.bin\n"
                      << "  " << argv[0] << " --check local_atlas.bin\n";
            return 2;
        }
        if (ROUTES != 160) throw std::runtime_error("unexpected routing input count");

        const std::string output_path = argv[1];
        std::ofstream output(output_path, std::ios::binary);
        if (!output) throw std::runtime_error("cannot create atlas: " + output_path);

        AtlasHeader header{};
        std::memcpy(header.magic, "SRBLAT2", 7);
        header.version = 1;
        header.radius = ATLAS_RADIUS;
        header.routes = ROUTES;
        header.cells = ATLAS_CELLS;
        header.sources = ROUTES;
        header.header_bytes = sizeof(AtlasHeader);
        header.values = static_cast<uint64_t>(ROUTES) * ATLAS_CELLS * ROUTES;
        output.write(reinterpret_cast<const char*>(&header), sizeof(header));

        const size_t state_count = static_cast<size_t>(SEARCH_CELLS) * ROUTES;
        std::vector<uint32_t> dist(state_count, INF32);
        std::vector<uint16_t> slab(static_cast<size_t>(ATLAS_CELLS) * ROUTES, INF16);
        RadixHeap heap;
        const auto begin = std::chrono::steady_clock::now();

        for (int source = 0; source < ROUTES; ++source) {
            run_one_source(source, dist, slab, heap);
            output.write(reinterpret_cast<const char*>(slab.data()),
                         static_cast<std::streamsize>(slab.size() * sizeof(uint16_t)));
            if (!output) throw std::runtime_error("failed while writing atlas");
            const double sec = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - begin).count();
            std::cerr << '\r' << "atlas " << (source + 1) << '/' << ROUTES
                      << " elapsed=" << std::fixed << std::setprecision(1) << sec << " s"
                      << " eta=" << (sec / (source + 1) * (ROUTES - source - 1)) << " s"
                      << std::flush;
        }
        output.flush();
        std::cerr << "\nwritten=" << output_path
                  << " values=" << header.values
                  << " bytes=" << (sizeof(header) + header.values * sizeof(uint16_t)) << "\n";
        return 0;
    } catch (const std::exception& exc) {
        std::cerr << "error: " << exc.what() << '\n';
        return 1;
    }
}
