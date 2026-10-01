#pragma once

#include "local_arch_data.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace srb_v5 {

struct Slice { const char* data = nullptr; size_t size = 0; };
struct Endpoint { int16_t x = 0, y = 0; uint16_t port = 0; };

class RadixHeap {
public:
    using Item = std::pair<uint32_t,uint32_t>;
    void clear() { for (auto& b : buckets_) b.clear(); last_ = 0; size_ = 0; }
    bool empty() const { return size_ == 0; }
    void push(uint32_t key, uint32_t state) { buckets_[index(key,last_)].push_back({key,state}); ++size_; }
    Item pop() {
        if (buckets_[0].empty()) pull();
        Item v = buckets_[0].back(); buckets_[0].pop_back(); --size_; return v;
    }
private:
    static int index(uint32_t a,uint32_t b) { uint32_t v=a^b; if(!v)return 0;
#if defined(__GNUC__)
        return 32-__builtin_clz(v);
#else
        int n=0;while(v){++n;v>>=1;}return n;
#endif
    }
    void pull() {
        int i=1;while(i<33&&buckets_[i].empty())++i;uint32_t n=std::numeric_limits<uint32_t>::max();
        for(const auto& v:buckets_[i]) n=std::min(n,v.first);
        last_=n;std::vector<Item> tmp;tmp.swap(buckets_[i]);
        for(const auto& v:tmp)buckets_[index(v.first,last_)].push_back(v);
    }
    std::array<std::vector<Item>,33> buckets_{}; uint32_t last_=0; uint64_t size_=0;
};

class ArchitectureEstimator {
public:
    ArchitectureEstimator() {
        const auto begin = std::chrono::steady_clock::now();
        build_runtime_geometry();
        build_runtime_atlas();
        derive_runtime_model();
        preprocess_seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        std::cerr << "runtime_preprocess=" << preprocess_seconds_ << " s\n";
    }

    bool predict_spec(Slice from, Slice to, uint32_t& result) {
        Endpoint a, b;
        if (!parse_endpoint(from, a) || !parse_endpoint(to, b)) return false;
        if (a.x == b.x && a.y == b.y && a.port == b.port) { result = 0; return true; }
        if (predict_local(a, b, result)) { ++local_queries_; return true; }
        result = predict_long(a, b);
        ++long_queries_;
        return true;
    }

    uint64_t local_queries() const { return local_queries_; }
    uint64_t long_queries() const { return long_queries_; }
    uint64_t block_queries() const { return block_queries_; }
    double preprocess_seconds() const { return preprocess_seconds_; }
    size_t memory_bytes() const {
        return atlas_.size() * sizeof(uint16_t) + cycle_.size() * sizeof(uint16_t) +
               bias_.size() * sizeof(int16_t) + geometry_.size() * sizeof(uint16_t);
    }
private:
    static constexpr uint32_t INF = std::numeric_limits<uint32_t>::max();
    struct Seed { uint16_t route; int dx, dy; uint16_t cost; };

    int atlas_radius_ = 0, atlas_width_ = 0;
    uint32_t atlas_cells_ = 0;
    std::vector<uint16_t> atlas_;
    std::vector<uint16_t> cycle_;
    std::vector<int16_t> bias_;
    std::vector<uint16_t> geometry_;
    int geometry_max_x_ = 0, geometry_max_y_ = 0, geometry_width_ = 0;
    uint64_t local_queries_ = 0, long_queries_ = 0;
    mutable uint64_t block_queries_ = 0;
    double preprocess_seconds_ = 0.0;

    void build_runtime_geometry() {
        using namespace srb_local_arch;
        constexpr int max_x = 119, max_y = 549, margin = 16;
        constexpr int rx = max_x + margin, ry = max_y + margin;
        constexpr int width = rx * 2 + 1, height = ry * 2 + 1;
        constexpr int out_width = max_x * 2 + 1, out_height = max_y * 2 + 1;
        const auto begin = std::chrono::steady_clock::now();

        const size_t state_count = static_cast<size_t>(width) * height * kRouteCount;
        std::vector<uint32_t> dist(state_count, INF);
        geometry_.assign(static_cast<size_t>(out_width) * out_height, UINT16_MAX);
        RadixHeap heap;
        const auto state_id = [=](int x, int y, int route) {
            return static_cast<uint32_t>((static_cast<uint64_t>(y) * width + x) * kRouteCount + route);
        };
        for (int route = 0; route < kRouteCount; ++route) {
            const uint32_t state = state_id(rx, ry, route);
            dist[state] = 0;
            heap.push(0, state);
        }

        uint64_t remaining = static_cast<uint64_t>(out_width) * out_height;
        uint64_t expanded = 0;
        while (!heap.empty() && remaining) {
            const auto item = heap.pop();
            const uint32_t d = item.first, state = item.second;
            if (dist[state] != d) continue;
            const int route = static_cast<int>(state % kRouteCount);
            const uint32_t cell = state / kRouteCount;
            const int x = static_cast<int>(cell % width), y = static_cast<int>(cell / width);
            ++expanded;
            const int dx = x - rx, dy = y - ry;
            if (std::abs(dx) <= max_x && std::abs(dy) <= max_y) {
                const size_t out = static_cast<size_t>(dy + max_y) * out_width + dx + max_x;
                if (geometry_[out] == UINT16_MAX) {
                    if (d >= UINT16_MAX) throw std::runtime_error("runtime geometry delay exceeds uint16");
                    geometry_[out] = static_cast<uint16_t>(d);
                    --remaining;
                }
            }
            const uint16_t input = kRouteToInput[route];
            for (uint32_t ei = kTransitionOffset[input]; ei < kTransitionOffset[input + 1]; ++ei) {
                const auto& edge = kTransitions[ei];
                const int nx = x + edge.dx, ny = y + edge.dy;
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                const uint32_t next = state_id(nx, ny, edge.next_route);
                const uint32_t nd = d + edge.cost;
                if (nd < dist[next]) { dist[next] = nd; heap.push(nd, next); }
            }
        }
        if (remaining) throw std::runtime_error("runtime geometry search box was insufficient");
        geometry_max_x_ = max_x;
        geometry_max_y_ = max_y;
        geometry_width_ = out_width;
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        std::cerr << "geometry_preprocess=" << seconds << " s expanded=" << expanded << '\n';
    }

    void build_runtime_atlas() {
        using namespace srb_local_arch;
        constexpr int radius = 56, search_margin = 8;
        constexpr int search_radius = radius + search_margin;
        constexpr int search_width = search_radius * 2 + 1;
        constexpr int atlas_width = radius * 2 + 1;
        constexpr int atlas_cells = atlas_width * atlas_width;
        constexpr int search_cells = search_width * search_width;
        const auto begin = std::chrono::steady_clock::now();

        atlas_radius_ = radius;
        atlas_width_ = atlas_width;
        atlas_cells_ = atlas_cells;
        const size_t slab_values = static_cast<size_t>(atlas_cells) * kRouteCount;
        atlas_.assign(static_cast<size_t>(kRouteCount) * slab_values, UINT16_MAX);
        std::vector<uint32_t> dist(static_cast<size_t>(search_cells) * kRouteCount, INF);
        RadixHeap heap;
        const auto state_id = [=](int x, int y, int route) {
            return static_cast<uint32_t>((static_cast<uint64_t>(y) * search_width + x) * kRouteCount + route);
        };

        for (int source = 0; source < kRouteCount; ++source) {
            std::fill(dist.begin(), dist.end(), INF);
            heap.clear();
            const uint32_t start = state_id(search_radius, search_radius, source);
            dist[start] = 0;
            heap.push(0, start);
            uint64_t remaining = static_cast<uint64_t>(atlas_cells) * kRouteCount;

            while (!heap.empty()) {
                const auto item = heap.pop();
                const uint32_t d = item.first, state = item.second;
                if (dist[state] != d) continue;
                const int route = static_cast<int>(state % kRouteCount);
                const int cell = static_cast<int>(state / kRouteCount);
                const int x = cell % search_width, y = cell / search_width;
                if (x >= search_margin && x < search_margin + atlas_width &&
                    y >= search_margin && y < search_margin + atlas_width) {
                    if (remaining > 0) --remaining;
                    if (remaining == 0) break;
                }
                const uint16_t input = kRouteToInput[route];
                for (uint32_t ei = kTransitionOffset[input]; ei < kTransitionOffset[input + 1]; ++ei) {
                    const auto& edge = kTransitions[ei];
                    const int nx = x + edge.dx, ny = y + edge.dy;
                    if (nx < 0 || nx >= search_width || ny < 0 || ny >= search_width) continue;
                    const uint32_t next = state_id(nx, ny, edge.next_route);
                    const uint32_t nd = d + edge.cost;
                    if (nd < dist[next]) { dist[next] = nd; heap.push(nd, next); }
                }
            }

            uint16_t* out = atlas_.data() + static_cast<size_t>(source) * slab_values;
            for (int dy = -radius; dy <= radius; ++dy) {
                const int y = search_radius + dy;
                for (int dx = -radius; dx <= radius; ++dx) {
                    const int x = search_radius + dx;
                    const size_t base = state_id(x, y, 0);
                    for (int route = 0; route < kRouteCount; ++route) {
                        const uint32_t value = dist[base + route];
                        *out++ = value >= UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(value);
                    }
                }
            }
            if ((source + 1) % 16 == 0 || source + 1 == kRouteCount) {
                const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
                std::cerr << "atlas_preprocess=" << (source + 1) << '/' << kRouteCount
                          << " elapsed=" << seconds << " s\n";
            }
        }
        cycle_.assign(static_cast<size_t>(atlas_width_) * atlas_width_, UINT16_MAX);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        std::cerr << "atlas_preprocess_done=" << seconds << " s\n";
    }

    uint16_t minimum_atlas_cost(int dx, int dy) const {
        uint16_t best = UINT16_MAX;
        for (int source = 0; source < srb_local_arch::kRouteCount; ++source)
            for (int target = 0; target < srb_local_arch::kRouteCount; ++target)
                best = std::min(best, atlas_get(source, dx, dy, target));
        return best;
    }

    void derive_runtime_model() {
        using namespace srb_local_arch;
        const auto begin = std::chrono::steady_clock::now();
        constexpr int signs[9][2] = {
            {0,0}, {1,0}, {-1,0}, {0,1}, {1,1}, {-1,1}, {0,-1}, {1,-1}, {-1,-1}
        };
        constexpr int scales[3] = {24, 34, 44};
        const size_t port_pairs = static_cast<size_t>(kPortCount) * kPortCount;
        bias_.assign(9 * port_pairs, 0);
        std::array<std::vector<int32_t>, 3> residuals;
        for (auto& v : residuals) v.resize(port_pairs);
        std::vector<uint32_t> best(kPortCount);
        std::array<uint32_t, kRouteCount> route_cost{};

        for (int q = 0; q < 9; ++q) {
            for (int sample = 0; sample < 3; ++sample) {
                const int dx = signs[q][0] * scales[sample];
                const int dy = signs[q][1] * scales[sample];
                const uint16_t base16 = q == 0 ? 0 : minimum_atlas_cost(dx, dy);
                if (base16 == UINT16_MAX) throw std::runtime_error("no Atlas route for model sample");
                const int32_t base = base16;

                for (int source_port = 0; source_port < kPortCount; ++source_port) {
                    std::fill(best.begin(), best.end(), INF);
                    Seed source_seeds[64];
                    const int seed_count = seeds(static_cast<uint16_t>(source_port), source_seeds);
                    for (int si = 0; si < seed_count; ++si) {
                        const Seed& seed = source_seeds[si];
                        const int rx = dx - seed.dx, ry = dy - seed.dy;
                        if (std::abs(rx) > atlas_radius_ || std::abs(ry) > atlas_radius_) continue;
                        for (int route = 0; route < kRouteCount; ++route) {
                            const uint16_t d = atlas_get(seed.route, rx, ry, route);
                            route_cost[route] = d == UINT16_MAX ? INF : static_cast<uint32_t>(seed.cost) + d;
                            if (route_cost[route] != INF) {
                                const uint16_t port = kInputPort[kRouteToInput[route]];
                                best[port] = std::min(best[port], route_cost[route]);
                            }
                        }
                        for (int target_port = 0; target_port < kPortCount; ++target_port) {
                            for (uint32_t ai = kTargetOffset[target_port]; ai < kTargetOffset[target_port + 1]; ++ai) {
                                const auto& arc = kTargetArcs[ai];
                                if (route_cost[arc.id] != INF)
                                    best[target_port] = std::min(best[target_port], route_cost[arc.id] + arc.cost);
                            }
                        }
                    }
                    const size_t row = static_cast<size_t>(source_port) * kPortCount;
                    for (int target_port = 0; target_port < kPortCount; ++target_port) {
                        int64_t value = best[target_port] == INF ? 0 : static_cast<int64_t>(best[target_port]) - base;
                        value = std::max<int64_t>(-32768, std::min<int64_t>(32767, value));
                        residuals[sample][row + target_port] = static_cast<int32_t>(value);
                    }
                }
            }
            int16_t* out = bias_.data() + static_cast<size_t>(q) * port_pairs;
            for (size_t i = 0; i < port_pairs; ++i) {
                int32_t a = residuals[0][i], b = residuals[1][i], c = residuals[2][i];
                if (a > b) std::swap(a, b);
                if (b > c) std::swap(b, c);
                if (a > b) std::swap(a, b);
                out[i] = static_cast<int16_t>(b);
            }
            std::cerr << "model_preprocess=" << (q + 1) << "/9\n";
        }
        const uint16_t x40 = minimum_atlas_cost(40, 0);
        if (x40 == UINT16_MAX) throw std::runtime_error("no Atlas route for horizontal unit cost");
        cycle_[static_cast<size_t>(atlas_radius_) * atlas_width_ + atlas_radius_ + 40] = x40;
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        std::cerr << "model_preprocess_done=" << seconds << " s\n";
    }

    uint16_t atlas_get(int source, int dx, int dy, int target) const {
        if (std::abs(dx) > atlas_radius_ || std::abs(dy) > atlas_radius_) return UINT16_MAX;
        const uint32_t cell = static_cast<uint32_t>((dy + atlas_radius_) * atlas_width_ + dx + atlas_radius_);
        return atlas_[(static_cast<size_t>(source) * atlas_cells_ + cell) * srb_local_arch::kRouteCount + target];
    }

    uint16_t cycle_get(int dx, int dy) const {
        if (std::abs(dx) > atlas_radius_ || std::abs(dy) > atlas_radius_) return UINT16_MAX;
        return cycle_[static_cast<size_t>(dy + atlas_radius_) * atlas_width_ + dx + atlas_radius_];
    }

    static void trim(Slice& s) {
        while (s.size && (s.data[0] == ' ' || s.data[0] == '\t' || s.data[0] == '\r')) { ++s.data; --s.size; }
        while (s.size && (s.data[s.size - 1] == ' ' || s.data[s.size - 1] == '\t' || s.data[s.size - 1] == '\r')) --s.size;
    }
    static bool number(Slice s, size_t& p, int& value) {
        if (p >= s.size || s.data[p] < '0' || s.data[p] > '9') return false;
        value = 0; while (p < s.size && s.data[p] >= '0' && s.data[p] <= '9') value = value * 10 + s.data[p++] - '0';
        return true;
    }
    static int compare(Slice a, const char* b) {
        const size_t n2 = std::strlen(b), n = std::min(a.size, n2);
        const int c = std::memcmp(a.data, b, n);
        return c ? c : (a.size < n2 ? -1 : (a.size > n2 ? 1 : 0));
    }
    static int find_port(Slice s) {
        int lo = 0, hi = srb_local_arch::kPortCount;
        while (lo < hi) { int m = (lo + hi) / 2, c = compare(s, srb_local_arch::kSortedPortNames[m]);
            if (!c) return srb_local_arch::kSortedPortIds[m];
            if (c < 0) hi = m; else lo = m + 1; }
        return -1;
    }
    static bool parse_endpoint(Slice s, Endpoint& out) {
        trim(s); if (s.size < 9 || std::memcmp(s.data, "SRB_", 4)) return false;
        size_t p = 4; int x, y; if (!number(s, p, x) || p >= s.size || s.data[p++] != '_' ||
            !number(s, p, y) || p >= s.size || s.data[p++] != '/') return false;
        if (x < 0 || x >= 120 || y < 0 || y >= 550) return false;
        const int port = find_port(Slice{s.data + p, s.size - p}); if (port < 0) return false;
        out = Endpoint{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<uint16_t>(port)}; return true;
    }

    static int seeds(uint16_t port, Seed* out) {
        using namespace srb_local_arch;
        const int input = kPortToInput[port]; int n = 0;
        if (input >= 0) {
            const int route = kInputToRoute[input];
            if (route >= 0) out[n++] = Seed{static_cast<uint16_t>(route), 0, 0, 0};
            else for (uint32_t i = kTransitionOffset[input]; i < kTransitionOffset[input + 1] && n < 64; ++i) {
                const auto& e = kTransitions[i]; out[n++] = Seed{e.next_route, e.dx, e.dy, e.cost};
            }
        } else {
            const auto& net = kOutputNet[port];
            if (net.next_route >= 0) out[n++] = Seed{static_cast<uint16_t>(net.next_route), net.dx, net.dy, 0};
        }
        return n;
    }

    static uint32_t direct_arc(int input, uint16_t target) {
        using namespace srb_local_arch;
        for (uint32_t i = kDirectOffset[input]; i < kDirectOffset[input + 1]; ++i)
            if (kDirectArcs[i].id == target) return kDirectArcs[i].cost;
        return INF;
    }

    bool block_free_box(const Endpoint& a, const Endpoint& b) const {
        using namespace srb_local_arch;
        const int lx = std::min(a.x, b.x), rx = std::max(a.x, b.x), ly = std::min(a.y, b.y), uy = std::max(a.y, b.y);
        for (int i = 0; i < kBlockCount; ++i) { const auto& g = kBlocks[i];
            if (lx <= g.right && rx >= g.left && ly <= g.upper && uy >= g.lower) return false; }
        return true;
    }

    bool predict_local(const Endpoint& a, const Endpoint& b, uint32_t& result) const {
        using namespace srb_local_arch;
        const int dx = b.x - a.x, dy = b.y - a.y;
        if (std::max(std::abs(dx), std::abs(dy)) > 48 || !block_free_box(a, b)) return false;
        Seed ss[64]; const int sn = seeds(a.port, ss); if (!sn) return false;
        uint32_t best = INF; const int target_input = kPortToInput[b.port];
        const int target_route = target_input >= 0 ? kInputToRoute[target_input] : -1;
        if (target_input >= 0 && target_route < 0) return false;
        if (dx == 0 && dy == 0 && kPortToInput[a.port] >= 0 && target_input < 0)
            best = direct_arc(kPortToInput[a.port], b.port);
        for (int si = 0; si < sn; ++si) {
            const int rx = dx - ss[si].dx, ry = dy - ss[si].dy;
            if (target_route >= 0) {
                const uint16_t d = atlas_get(ss[si].route, rx, ry, target_route);
                if (d != UINT16_MAX) best = std::min(best, static_cast<uint32_t>(ss[si].cost) + d);
            } else for (uint32_t i = kTargetOffset[b.port]; i < kTargetOffset[b.port + 1]; ++i) {
                const auto& t = kTargetArcs[i]; const uint16_t d = atlas_get(ss[si].route, rx, ry, t.id);
                if (d != UINT16_MAX) best = std::min(best, static_cast<uint32_t>(ss[si].cost) + d + t.cost);
            }
        }
        if (best == INF) return false;
        best += std::abs(static_cast<int>(kXGapPrefix[b.x]) - kXGapPrefix[a.x]);
        best += std::abs(static_cast<int>(kYGapPrefix[b.y]) - kYGapPrefix[a.y]);
        result = best; return true;
    }

    static int quadrant(int dx, int dy) {
        return (dx > 0 ? 1 : 0) + (dx < 0 ? 2 : 0) + (dy > 0 ? 3 : 0) + (dy < 0 ? 6 : 0);
    }


    double geometry(int dx, int dy) const {
        if (std::abs(dx) > geometry_max_x_ || std::abs(dy) > geometry_max_y_)
            return 20.0 * (std::abs(dx) + std::abs(dy));
        return geometry_[static_cast<size_t>(dy + geometry_max_y_) * geometry_width_ + dx + geometry_max_x_];
    }

    uint32_t predict_long(const Endpoint& a, const Endpoint& b) const {
        using namespace srb_local_arch;
        int dx = b.x - a.x, dy = b.y - a.y;
        double value = geometry(dx, dy);
        const int bin = quadrant(dx, dy);
        value += bias_[(static_cast<size_t>(bin) * kPortCount + a.port) * kPortCount + b.port];
        value += std::abs(static_cast<int>(kXGapPrefix[b.x]) - kXGapPrefix[a.x]);
        value += std::abs(static_cast<int>(kYGapPrefix[b.y]) - kYGapPrefix[a.y]);

        const double x_unit = static_cast<double>(cycle_get(40, 0)) / 40.0;
        int shared_vertical_detour = std::numeric_limits<int>::max();
        bool any_block = false;
        for (int i = 1; i < kBlockCount; ++i) {
            const auto& g = kBlocks[i]; bool affected = false;
            const bool opposite_x = (a.x < g.left && b.x > g.right) || (b.x < g.left && a.x > g.right);
            if (opposite_x && a.y >= g.lower && a.y <= g.upper && b.y >= g.lower && b.y <= g.upper && g.hpass) {
                value += g.hdelay - x_unit * (g.right - g.left + 1); affected = true;
            }
            const bool crosses_y = (a.y < g.lower && b.y > g.upper) || (b.y < g.lower && a.y > g.upper);
            if (crosses_y && a.x >= g.left && a.x <= g.right && b.x >= g.left && b.x <= g.right && !g.vpass) {
                const int left_detour = (a.x - g.left + 1) + (b.x - g.left + 1);
                const int right_detour = (g.right - a.x + 1) + (g.right - b.x + 1);
                shared_vertical_detour = std::min(shared_vertical_detour, std::min(left_detour, right_detour));
                affected = true;
            }
            if (affected) any_block = true;
        }
        if (shared_vertical_detour != std::numeric_limits<int>::max())
            value += x_unit * shared_vertical_detour;
        if (any_block) ++block_queries_;
        if (!(value > 0.0)) return 0;
        return static_cast<uint32_t>(std::min<double>(std::numeric_limits<uint32_t>::max(), std::floor(value + 0.5)));
    }
};

} // namespace srb_v5
