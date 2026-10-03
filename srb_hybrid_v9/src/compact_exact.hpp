#pragma once

#include "compact_graph_data.hpp"
#ifdef V9_COMPACT_LANDMARKS
#include "compact_landmark_data.hpp"
#endif
#include "srb_fast.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <vector>

namespace v9_compact {

using namespace v9_compact_data;
inline constexpr uint32_t kInfinity = std::numeric_limits<uint32_t>::max() / 4;

struct Result {
    bool reachable = false;
    bool budget_exhausted = false;
    uint32_t delay = kInfinity;
    uint32_t expanded = 0;
};

struct HeapItem {
    uint32_t priority;
    uint32_t distance;
    uint32_t state;
};

class TinyHeap {
public:
    void clear() { values_.clear(); }
    bool empty() const { return values_.empty(); }
    const HeapItem& top() const { return values_.front(); }

    void push(HeapItem value) {
        values_.push_back(value);
        std::size_t index = values_.size() - 1;
        while (index) {
            const std::size_t parent = (index - 1) >> 1;
            if (!less(value, values_[parent])) break;
            values_[index] = values_[parent];
            index = parent;
        }
        values_[index] = value;
    }

    HeapItem pop() {
        const HeapItem result = values_.front();
        const HeapItem value = values_.back();
        values_.pop_back();
        if (values_.empty()) return result;
        std::size_t index = 0;
        while (true) {
            const std::size_t left = index * 2 + 1;
            if (left >= values_.size()) break;
            const std::size_t right = left + 1;
            const std::size_t child = right < values_.size() && less(values_[right], values_[left])
                ? right : left;
            if (!less(values_[child], value)) break;
            values_[index] = values_[child];
            index = child;
        }
        values_[index] = value;
        return result;
    }

private:
    static bool less(const HeapItem& a, const HeapItem& b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        if (a.distance != b.distance) return a.distance > b.distance;
        return a.state < b.state;
    }
    std::vector<HeapItem> values_;
};

class Solver {
public:
    Solver() {
        heap_.clear();
    }

    Result query(const srb_fast::Endpoint& source,
                 const srb_fast::Endpoint& target,
                 uint32_t max_expanded) {
        Result result;
        if (!valid_cell(source.x, source.y) || !valid_cell(target.x, target.y) ||
            source.port >= kPortCount || target.port >= kPortCount) return result;
        if (source.x == target.x && source.y == target.y && source.port == target.port) {
            result.reachable = true;
            result.delay = 0;
            return result;
        }

        begin_query();
        heap_.clear();
        table_overflow_ = false;
        target_port_ = target.port;
#ifdef V9_COMPACT_LANDMARKS
        select_landmarks(source.x, source.y, target.x, target.y);
#endif
        uint32_t best = kInfinity;
        const bool target_is_input = kPortIsInput[target.port] != 0;
        int16_t target_route = -1;
        if (target_is_input) {
            target_route = kInputToRoute[static_cast<uint16_t>(kPortToInput[target.port])];
            if (target_route < 0) return result;
        }

        if (kPortIsInput[source.port]) {
            const uint16_t input = static_cast<uint16_t>(kPortToInput[source.port]);
            const int16_t route = kInputToRoute[input];
            if (route >= 0) {
                relax(pack(source.x, source.y, static_cast<uint16_t>(route)), 0);
            } else {
                if (!target_is_input && source.x == target.x && source.y == target.y) {
                    const uint16_t direct = arc_delay(input, target.port);
                    if (direct != kNoU16) best = direct;
                }
                for_each_transition(input, [&](const Edge& edge) {
                    int nx = 0, ny = 0;
                    uint32_t extra = 0;
                    if (spatial_move(source.x, source.y, edge.net_id, nx, ny, extra)) {
                        relax(pack(nx, ny, edge.next_input), edge.arc_delay + extra);
                    }
                });
            }
        } else {
            const int16_t net = kNetByOutput[source.port];
            if (net >= 0) {
                int nx = 0, ny = 0;
                uint32_t extra = 0;
                if (spatial_move(source.x, source.y, static_cast<uint16_t>(net), nx, ny, extra)) {
                    relax(pack(nx, ny, kNets[net].dst_input), extra);
                }
            }
        }

        while (!heap_.empty()) {
            const HeapItem item = heap_.pop();
            if (distance(item.state) != item.distance) continue;
            if (item.priority >= best) break;
            if (result.expanded >= max_expanded) {
                result.budget_exhausted = true;
                return result;
            }
            ++result.expanded;
            int x = 0, y = 0;
            uint16_t route = 0;
            unpack(item.state, x, y, route);
            const uint16_t input = kRouteToInput[route];
            if (target_is_input && x == target.x && y == target.y && route == target_route) {
                best = item.distance;
                break;
            }
            if (!target_is_input && x == target.x && y == target.y) {
                const uint16_t final_arc = arc_delay(input, target.port);
                if (final_arc != kNoU16) best = std::min(best, item.distance + final_arc);
            }
            for_each_transition(input, [&](const Edge& edge) {
                int nx = 0, ny = 0;
                uint32_t extra = 0;
                if (!spatial_move(x, y, edge.net_id, nx, ny, extra)) return;
                const uint32_t next_distance = item.distance + edge.arc_delay + extra;
                if (next_distance < best) relax(pack(nx, ny, edge.next_input), next_distance);
            });
            if (table_overflow_) {
                result.budget_exhausted = true;
                return result;
            }
        }

        if (best != kInfinity) {
            result.reachable = true;
            result.delay = best;
        }
        return result;
    }

private:
    static constexpr uint32_t kTableSize = 4096;
    uint32_t keys_[kTableSize]{};
    uint32_t distances_[kTableSize]{};
    uint32_t stamps_[kTableSize]{};
    uint32_t generation_ = 0;
    uint16_t target_port_ = 0;
    bool table_overflow_ = false;
    TinyHeap heap_;
#ifdef V9_COMPACT_LANDMARKS
    static constexpr int kActiveLandmarks = 4;
    uint8_t active_landmarks_[kActiveLandmarks]{};
    uint16_t active_from_target_[kActiveLandmarks]{};
    uint16_t active_to_target_[kActiveLandmarks]{};
#endif
    void begin_query() {
        if (++generation_ == 0) {
            std::fill(std::begin(stamps_), std::end(stamps_), 0);
            generation_ = 1;
        }
    }

    static uint32_t pack(int x, int y, uint16_t route) {
        return (static_cast<uint32_t>(y * kWidth + x) * kRouteCount) + route;
    }

    static void unpack(uint32_t state, int& x, int& y, uint16_t& route) {
        route = static_cast<uint16_t>(state % kRouteCount);
        const uint32_t cell = state / kRouteCount;
        x = static_cast<int>(cell % kWidth);
        y = static_cast<int>(cell / kWidth);
    }

    static uint32_t hash(uint32_t state) {
        return (state * 2654435761u) & (kTableSize - 1);
    }

    uint32_t slot(uint32_t state, bool create) {
        uint32_t index = hash(state);
        for (uint32_t probe = 0; probe < kTableSize; ++probe) {
            if (stamps_[index] != generation_) {
                if (!create) return kTableSize;
                stamps_[index] = generation_;
                keys_[index] = state;
                distances_[index] = kInfinity;
                return index;
            }
            if (keys_[index] == state) return index;
            index = (index + 1) & (kTableSize - 1);
        }
        return kTableSize;
    }

    uint32_t distance(uint32_t state) {
        const uint32_t index = slot(state, false);
        return index == kTableSize ? kInfinity : distances_[index];
    }

    void relax(uint32_t state, uint32_t value) {
        const uint32_t index = slot(state, true);
        if (index == kTableSize) {
            table_overflow_ = true;
            return;
        }
        if (value >= distances_[index]) return;
        distances_[index] = value;
        uint32_t lower = kPortRouteLower[static_cast<uint32_t>(target_port_) * kRouteCount +
                                         state % kRouteCount];
        if (lower == kNoU16) lower = 0;
#ifdef V9_COMPACT_LANDMARKS
        lower = std::max(lower, landmark_lower(state / kRouteCount));
#endif
        heap_.push(HeapItem{value + lower, value, state});
    }

#ifdef V9_COMPACT_LANDMARKS
    void select_landmarks(int source_x, int source_y, int target_x, int target_y) {
        struct Candidate { uint16_t score; uint8_t landmark; };
        Candidate candidates[v9_compact_landmarks::kLandmarkCount]{};
        const uint32_t source = static_cast<uint32_t>(source_y * kWidth + source_x);
        const uint32_t target = static_cast<uint32_t>(target_y * kWidth + target_x);
        for (uint8_t landmark = 0; landmark < v9_compact_landmarks::kLandmarkCount; ++landmark) {
            const uint32_t base = static_cast<uint32_t>(landmark) *
                                  v9_compact_landmarks::kDenseCellCount;
            const uint16_t from_source = v9_compact_landmarks::kFrom[base + source];
            const uint16_t from_target = v9_compact_landmarks::kFrom[base + target];
            const uint16_t to_source = v9_compact_landmarks::kTo[base + source];
            const uint16_t to_target = v9_compact_landmarks::kTo[base + target];
            uint16_t score = 0;
            if (from_source != kNoU16 && from_target != kNoU16 && from_target > from_source)
                score = static_cast<uint16_t>(from_target - from_source);
            if (to_source != kNoU16 && to_target != kNoU16 && to_source > to_target)
                score = std::max<uint16_t>(score, static_cast<uint16_t>(to_source - to_target));
            candidates[landmark] = Candidate{score, landmark};
        }
        std::sort(std::begin(candidates), std::end(candidates),
                  [](const Candidate& first, const Candidate& second) {
                      if (first.score != second.score) return first.score > second.score;
                      return first.landmark < second.landmark;
                  });
        for (int index = 0; index < kActiveLandmarks; ++index) {
            const uint8_t landmark = candidates[index].landmark;
            const uint32_t base = static_cast<uint32_t>(landmark) *
                                  v9_compact_landmarks::kDenseCellCount;
            active_landmarks_[index] = landmark;
            active_from_target_[index] = v9_compact_landmarks::kFrom[base + target];
            active_to_target_[index] = v9_compact_landmarks::kTo[base + target];
        }
    }

    uint32_t landmark_lower(uint32_t dense_cell) const {
        uint32_t lower = 0;
        for (int index = 0; index < kActiveLandmarks; ++index) {
            const uint32_t base = static_cast<uint32_t>(active_landmarks_[index]) *
                                  v9_compact_landmarks::kDenseCellCount;
            const uint16_t from_cell = v9_compact_landmarks::kFrom[base + dense_cell];
            const uint16_t to_cell = v9_compact_landmarks::kTo[base + dense_cell];
            const uint16_t from_target = active_from_target_[index];
            const uint16_t to_target = active_to_target_[index];
            if (from_cell != kNoU16 && from_target != kNoU16 && from_target > from_cell)
                lower = std::max<uint32_t>(lower, from_target - from_cell);
            if (to_cell != kNoU16 && to_target != kNoU16 && to_cell > to_target)
                lower = std::max<uint32_t>(lower, to_cell - to_target);
        }
        return lower;
    }
#endif

    template <class Function>
    static void for_each_transition(uint16_t input, Function&& function) {
        for (uint16_t index = kTransitionOffsets[input]; index < kTransitionOffsets[input + 1]; ++index)
            function(kTransitions[index]);
    }

    static uint16_t arc_delay(uint16_t input, uint16_t output_port) {
        return kArcToOutput[static_cast<uint32_t>(input) * kPortCount + output_port];
    }

    static int block_at(int x, int y) {
        for (int index = 0; index < static_cast<int>(sizeof(kBlocks) / sizeof(kBlocks[0])); ++index) {
            const Block& block = kBlocks[index];
            if (x >= block.left && x <= block.right && y >= block.lower && y <= block.upper)
                return index;
        }
        return -1;
    }

    static bool valid_cell(int x, int y) {
        return x >= 0 && x < kWidth && y >= 0 && y < kHeight && block_at(x, y) < 0;
    }

    static uint32_t line_penalty(int x1, int y1, int x2, int y2) {
        uint32_t result = 0;
        for (const GapLine& line : kGapLines) {
            if (line.vertical && y1 == y2) {
                const int lower = std::min(x1, x2);
                const int upper = std::max(x1, x2);
                if (lower <= line.site && line.site < upper) result += line.delay;
            } else if (!line.vertical && x1 == x2) {
                const int lower = std::min(y1, y2);
                const int upper = std::max(y1, y2);
                if (lower <= line.site && line.site < upper) result += line.delay;
            }
        }
        return result;
    }

    static bool spatial_move(int source_x, int source_y, uint16_t net_id,
                             int& target_x, int& target_y, uint32_t& extra) {
        const Net& net = kNets[net_id];
        const bool horizontal = net.dx != 0;
        const int delta = horizontal ? net.dx : net.dy;
        const int step = delta > 0 ? 1 : -1;
        const int need = std::abs(delta);
        int x = source_x;
        int y = source_y;
        int counted = 0;
        uint32_t block_extra = 0;
        uint32_t charged = 0;
        while (counted < need) {
            if (horizontal) x += step;
            else y += step;
            if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return false;
            if (valid_cell(x, y)) {
                ++counted;
                continue;
            }
            const int block_index = block_at(x, y);
            bool crossable = false;
            uint16_t delay = 0;
            if (block_index >= 0) {
                const Block& block = kBlocks[block_index];
                crossable = horizontal ? block.horizontal_crossable : block.vertical_crossable;
                delay = horizontal ? block.horizontal_delay : block.vertical_delay;
            }
            if (crossable) {
                const uint32_t mask = 1u << block_index;
                if ((charged & mask) == 0) {
                    charged |= mask;
                    block_extra += delay;
                }
            } else {
                ++counted;
            }
        }
        if (!valid_cell(x, y)) return false;
        target_x = x;
        target_y = y;
        extra = block_extra + line_penalty(source_x, source_y, x, y);
        return true;
    }
};

} // namespace v9_compact
