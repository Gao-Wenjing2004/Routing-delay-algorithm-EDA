#pragma once

#include "p2_estimator.hpp"
#include "p6_embedded_model_data.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

extern "C" {
extern const unsigned char p6_data_stamp[];
extern const unsigned char p6_axis_begin[];
extern const unsigned char p6_axis_end[];
extern const unsigned char p6_beams_begin[];
extern const unsigned char p6_beams_end[];
extern const unsigned char p6_top_edges_begin[];
extern const unsigned char p6_top_edges_end[];
}

namespace p6_runtime {

constexpr int kStates = 160;
constexpr int kRadius = 32;
constexpr int kAxisWidth = 65;
constexpr int kStateBeam = 32;
constexpr int kEdgeBeam = 32;
constexpr int kCandidateLimit = 5;
constexpr uint16_t kInf16 = 65535;
constexpr uint32_t kBig = 1000000000u;

struct Result {
    uint32_t delay = 0;
    uint64_t transitions = 0;
    uint16_t candidates = 0;
    uint16_t valid_candidates = 0;
    bool reachable = false;
};

struct Source {
    int state = -1;
    int dx = 0;
    int dy = 0;
    int cost = 0;
};

struct TemplateCandidate {
    std::array<char, 10> axes{};
    std::array<int8_t, 10> local{};
    int8_t h_trunk = -1;
    int8_t v_trunk = -1;
    uint8_t runs = 0;
};

class MemoryReader {
public:
    MemoryReader(const unsigned char* begin, const unsigned char* end)
        : current_(begin), end_(end) {}

    template <class T>
    T read() {
        static_assert(std::is_trivially_copyable<T>::value, "binary scalar required");
        if (static_cast<std::size_t>(end_ - current_) < sizeof(T))
            throw std::runtime_error("truncated embedded P6 table");
        T value{};
        std::memcpy(&value, current_, sizeof(T));
        current_ += sizeof(T);
        return value;
    }

    const unsigned char* take(std::size_t size) {
        if (static_cast<std::size_t>(end_ - current_) < size)
            throw std::runtime_error("truncated embedded P6 table");
        const unsigned char* result = current_;
        current_ += size;
        return result;
    }

    bool finished() const { return current_ == end_; }

private:
    const unsigned char* current_ = nullptr;
    const unsigned char* end_ = nullptr;
};

struct BeamView {
    const uint16_t* data = nullptr;
    std::size_t count = 0;
    const uint16_t* begin() const { return data; }
    const uint16_t* end() const { return data + count; }
    std::size_t size() const { return count; }
};

class StaticGenerator {
public:
    StaticGenerator() { load(); }

    bool selected(const p2_runtime::DetailedPrediction& prediction) const {
        const int cheb = prediction.environment.cheb;
        const int band = cheb <= 2 ? 0 : cheb <= 4 ? 1 : cheb <= 8 ? 2 : cheb <= 16 ? 3 : 4;
        return p6_embedded::selected(
            prediction.source.port, prediction.environment.direction, band);
    }

    BeamView generate(uint16_t source_port, uint16_t target_port, int direction, int cheb) const {
        const int band = cheb <= 8 ? 0 : cheb <= 16 ? 1 : 2;
        const uint16_t source_stem = p6_embedded::kPortToStem[source_port];
        const uint16_t target_stem = p6_embedded::kPortToStem[target_port];
        const std::size_t map_index =
            (static_cast<std::size_t>(source_stem) * 9 + direction) * 3 + band;
        const int group = group_map_[map_index];
        if (group < 0) return {};
        const std::size_t offset =
            (static_cast<std::size_t>(group) * stem_count_ + target_stem) * kCandidateLimit;
        return BeamView{beams_.data() + offset, kCandidateLimit};
    }

    const TemplateCandidate& candidate(uint16_t id) const { return templates_[id]; }

private:
    std::vector<TemplateCandidate> templates_;
    std::vector<int16_t> group_map_;
    std::vector<uint16_t> beams_;
    uint16_t stem_count_ = 0;

    void load() {
        MemoryReader stream(p6_beams_begin, p6_beams_end);
        const unsigned char* magic = stream.take(8);
        if (std::memcmp(magic, "P6SBM001", 8) != 0)
            throw std::runtime_error("bad embedded P6 Beam5 table");
        const uint16_t template_count = stream.read<uint16_t>();
        templates_.resize(template_count);
        for (TemplateCandidate& candidate : templates_) {
            candidate.runs = stream.read<uint8_t>();
            candidate.h_trunk = stream.read<int8_t>();
            candidate.v_trunk = stream.read<int8_t>();
            const uint16_t axes = stream.read<uint16_t>();
            if (candidate.runs > candidate.axes.size())
                throw std::runtime_error("bad embedded P6 Beam5 template width");
            for (int index = 0; index < 10; ++index) {
                candidate.axes[index] = (axes & (UINT16_C(1) << index)) ? 'V' : 'H';
                candidate.local[index] = stream.read<int8_t>();
            }
        }
        stem_count_ = stream.read<uint16_t>();
        const uint16_t group_count = stream.read<uint16_t>();
        if (stem_count_ != 216)
            throw std::runtime_error("embedded P6 Beam5 stem mismatch");
        group_map_.resize(static_cast<std::size_t>(stem_count_) * 9 * 3);
        for (int16_t& group : group_map_) group = stream.read<int16_t>();
        beams_.resize(static_cast<std::size_t>(group_count) * stem_count_ * kCandidateLimit);
        for (uint16_t& id : beams_) {
            id = stream.read<uint16_t>();
            if (id >= template_count)
                throw std::runtime_error("bad embedded P6 Beam5 template ID");
        }
        if (!stream.finished()) throw std::runtime_error("trailing embedded P6 Beam5 data");
    }
};

class Solver {
public:
    Solver() {
        if (!p6_data_stamp[0]) throw std::runtime_error("missing embedded P6 data stamp");
        const std::size_t expected = static_cast<std::size_t>(2) * kStates * kAxisWidth * kStates * 2;
        if (static_cast<std::size_t>(p6_axis_end - p6_axis_begin) != expected)
            throw std::runtime_error("bad embedded P6 axis table size");
        const std::size_t top_expected =
            static_cast<std::size_t>(2) * kStates * kAxisWidth * kEdgeBeam;
        if (static_cast<std::size_t>(p6_top_edges_end - p6_top_edges_begin) != top_expected)
            throw std::runtime_error("bad embedded P6 top-edge table size");
    }

    bool selected(const p2_runtime::DetailedPrediction& prediction) const {
        return generator_.selected(prediction);
    }

    Result query(const p2_runtime::DetailedPrediction& prediction) {
        Result result;
        const auto& ids = generator_.generate(
            prediction.source.port, prediction.target.port,
            prediction.environment.direction, prediction.environment.cheb);
        result.candidates = static_cast<uint16_t>(ids.size());
        const uint32_t delay = price(
            prediction.source, prediction.target, ids,
            result.transitions, result.valid_candidates);
        result.reachable = delay < kBig;
        result.delay = result.reachable ? delay : 0;
        return result;
    }

private:
    StaticGenerator generator_;

    const uint16_t* table(int axis, int state, int delta) const {
        const std::size_t one_axis = static_cast<std::size_t>(kStates) * kAxisWidth * kStates;
        const std::size_t index = axis * one_axis +
            (static_cast<std::size_t>(state) * kAxisWidth + delta + kRadius) * kStates;
        return reinterpret_cast<const uint16_t*>(p6_axis_begin) + index;
    }

    const unsigned char* top_edges(int axis, int state, int delta) const {
        const std::size_t base = (((static_cast<std::size_t>(axis) * kStates + state) *
                                   kAxisWidth + delta + kRadius) * kEdgeBeam);
        return p6_top_edges_begin + base;
    }

    int source_candidates(int port, std::array<Source, 64>& result) const {
        int count = 0;
        const int iid = p6_embedded::kPortToInput[port];
        if (iid >= 0) {
            const int route = p6_embedded::kInputToState[iid];
            if (route >= 0) {
                result[0] = Source{route, 0, 0, 0};
                return 1;
            }
            for (uint16_t index = p6_embedded::kDirectOffsets[iid];
                 index < p6_embedded::kDirectOffsets[iid + 1]; ++index) {
                const auto& arc = p6_embedded::kDirectArcs[index];
                const auto& net = p6_embedded::kOutputNets[arc.id];
                if (net.route >= 0 && count < static_cast<int>(result.size()))
                    result[count++] = Source{net.route, net.dx, net.dy, arc.cost};
            }
            return count;
        }
        const auto& net = p6_embedded::kOutputNets[port];
        if (net.route >= 0) result[count++] = Source{net.route, net.dx, net.dy, 0};
        return count;
    }

    bool segment_extra(int x, int y, char axis, int delta, uint32_t& extra) const {
        const int nx = x + (axis == 'H' ? delta : 0);
        const int ny = y + (axis == 'V' ? delta : 0);
        if (nx < 0 || nx >= p6_embedded::kWidth || ny < 0 || ny >= p6_embedded::kHeight)
            return false;
        const int min_x = std::min(x, nx), max_x = std::max(x, nx);
        const int min_y = std::min(y, ny), max_y = std::max(y, ny);
        for (const auto& block : p6_embedded::kBlocks)
            if (min_x <= block.right && max_x >= block.left &&
                min_y <= block.upper && max_y >= block.lower) return false;
        extra = 0;
        for (const auto& gap : p6_embedded::kGaps) {
            if (axis == 'H' && gap.vertical && min_x <= gap.site && gap.site < max_x)
                extra += gap.delay;
            else if (axis == 'V' && !gap.vertical && min_y <= gap.site && gap.site < max_y)
                extra += gap.delay;
        }
        return true;
    }

    bool instantiate(
        const TemplateCandidate& candidate, int required_dx, int required_dy,
        std::array<int, 10>& values) const {
        for (int index = 0; index < candidate.runs; ++index) values[index] = candidate.local[index];
        for (const auto& item : {std::pair<char, int>{'H', required_dx}, {'V', required_dy}}) {
            const int trunk = item.first == 'H' ? candidate.h_trunk : candidate.v_trunk;
            bool found = false;
            int sum = 0;
            for (int index = 0; index < candidate.runs; ++index) {
                if (candidate.axes[index] != item.first) continue;
                if (index == trunk) found = true;
                else sum += values[index];
            }
            if (!found) {
                if (item.second != 0) return false;
            } else {
                values[trunk] = item.second - sum;
            }
        }
        for (int index = 0; index < candidate.runs; ++index)
            if (values[index] < -kRadius || values[index] > kRadius) return false;
        return true;
    }

    uint32_t price_candidate(
        int sx, int sy, int total_dx, int total_dy, const Source& source,
        const p6_embedded::Arc* targets, int target_count,
        const TemplateCandidate& candidate, const std::array<int, 10>& deltas,
        uint64_t& transitions) const {
        const char entry_axis = source.dx ? 'H' : source.dy ? 'V' : 0;
        if (entry_axis && (!candidate.runs || candidate.axes[0] != entry_axis)) return kBig;
        uint32_t entry_extra = 0;
        if (entry_axis && !segment_extra(
                sx, sy, entry_axis, source.dx ? source.dx : source.dy, entry_extra)) return kBig;

        std::array<uint32_t, kStates> costs, next;
        std::array<int, kStates> active{}, next_active{};
        costs.fill(kBig);
        costs[source.state] = static_cast<uint32_t>(source.cost) + entry_extra;
        active[0] = source.state;
        int active_count = 1;
        int x = source.dx, y = source.dy;
        for (int run = 0; run < candidate.runs; ++run) {
            const char axis = candidate.axes[run];
            const int delta = deltas[run];
            const bool source_noop = run == 0 && entry_axis == axis && delta == 0;
            if (!source_noop) {
                uint32_t extra = 0;
                if (!segment_extra(sx + x, sy + y, axis, delta, extra)) return kBig;
                next.fill(kBig);
                const bool final_run = run + 1 == candidate.runs;
                if (final_run) {
                    uint32_t best = kBig;
                    for (int active_index = 0; active_index < active_count; ++active_index) {
                        const int source_state = active[active_index];
                        const uint16_t* edges = table(axis == 'H' ? 0 : 1, source_state, delta);
                        transitions += target_count;
                        for (int target_index = 0; target_index < target_count; ++target_index) {
                            const auto& target = targets[target_index];
                            const uint16_t edge = edges[target.id];
                            if (edge != kInf16)
                                best = std::min(best, costs[source_state] + edge + extra + target.cost);
                        }
                    }
                    if (axis == 'H') x += delta; else y += delta;
                    return x == total_dx && y == total_dy ? best : kBig;
                }
                for (int active_index = 0; active_index < active_count; ++active_index) {
                    const int source_state = active[active_index];
                    const uint16_t* edges = table(axis == 'H' ? 0 : 1, source_state, delta);
                    const unsigned char* choices = top_edges(
                        axis == 'H' ? 0 : 1, source_state, delta);
                    transitions += kEdgeBeam;
                    for (int edge_index = 0; edge_index < kEdgeBeam; ++edge_index) {
                        const uint16_t target = choices[edge_index];
                        const uint16_t edge = edges[target];
                        if (edge != kInf16)
                            next[target] = std::min(
                                next[target], costs[source_state] + edge + extra);
                    }
                }
                int next_count = 0;
                for (int state = 0; state < kStates; ++state)
                    if (next[state] < kBig) next_active[next_count++] = state;
                if (next_count > kStateBeam) {
                    std::nth_element(
                        next_active.begin(), next_active.begin() + kStateBeam,
                        next_active.begin() + next_count,
                        [&next](int left, int right) {
                            return std::pair<uint32_t, int>{next[left], left} <
                                   std::pair<uint32_t, int>{next[right], right};
                        });
                    next_count = kStateBeam;
                }
                costs = next;
                std::copy_n(next_active.begin(), next_count, active.begin());
                active_count = next_count;
                if (!active_count) return kBig;
            }
            if (axis == 'H') x += delta; else y += delta;
        }
        if (x != total_dx || y != total_dy) return kBig;
        uint32_t best = kBig;
        for (int target_index = 0; target_index < target_count; ++target_index) {
            const auto& target = targets[target_index];
            if (costs[target.id] < kBig)
                best = std::min(best, costs[target.id] + target.cost);
        }
        return best;
    }

    uint32_t price(
        const srb_fast::Endpoint& from, const srb_fast::Endpoint& to,
        const BeamView& candidate_ids,
        uint64_t& transitions, uint16_t& valid_candidates) const {
        const int total_dx = static_cast<int>(to.x) - from.x;
        const int total_dy = static_cast<int>(to.y) - from.y;
        std::array<Source, 64> sources{};
        const int source_count = source_candidates(from.port, sources);
        const uint16_t target_begin = p6_embedded::kTargetOffsets[to.port];
        const uint16_t target_end = p6_embedded::kTargetOffsets[to.port + 1];
        const p6_embedded::Arc* targets = p6_embedded::kTargetArcs + target_begin;
        const int target_count = target_end - target_begin;
        uint32_t best = from.x == to.x && from.y == to.y && from.port == to.port ? 0u : kBig;
        const int iid = p6_embedded::kPortToInput[from.port];
        if (from.x == to.x && from.y == to.y && iid >= 0) {
            for (uint16_t index = p6_embedded::kDirectOffsets[iid];
                 index < p6_embedded::kDirectOffsets[iid + 1]; ++index) {
                const auto& arc = p6_embedded::kDirectArcs[index];
                if (arc.id == to.port) best = std::min(best, static_cast<uint32_t>(arc.cost));
            }
        }
        std::array<int, 10> deltas{};
        for (uint16_t candidate_id : candidate_ids) {
            const TemplateCandidate& candidate = generator_.candidate(candidate_id);
            for (int source_index = 0; source_index < source_count; ++source_index) {
                const Source& source = sources[source_index];
                if (!instantiate(candidate, total_dx - source.dx, total_dy - source.dy, deltas))
                    continue;
                const uint32_t priced = price_candidate(
                    from.x, from.y, total_dx, total_dy, source, targets, target_count,
                    candidate, deltas, transitions);
                if (priced < kBig) {
                    ++valid_candidates;
                    best = std::min(best, priced);
                }
            }
        }
        return best;
    }
};

} // namespace p6_runtime
