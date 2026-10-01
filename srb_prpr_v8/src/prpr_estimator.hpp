#pragma once

#include "../include/prpr_model_data.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <tuple>
#include <vector>

namespace prpr {

struct Slice {
    const char* data = nullptr;
    std::size_t size = 0;
};

struct Endpoint {
    int16_t x = 0;
    int16_t y = 0;
    uint16_t port = 0;
};

struct Candidate {
    double delay = std::numeric_limits<double>::infinity();
    int remainder = std::numeric_limits<int>::max();
};

class Estimator {
public:
    Estimator() {
        std::fill(std::begin(port_slots_), std::end(port_slots_), int16_t{-1});
        for (int i = 0; i < prpr_data::kPortCount; ++i) {
            const char* name = prpr_data::kSortedPortNames[i];
            const auto length = static_cast<uint16_t>(std::strlen(name));
            const int pid = prpr_data::kSortedPortIds[i];
            port_lengths_[pid] = length;
            port_names_[pid] = name;
            uint32_t slot = hash_port({name, length}) & (kHashSlots - 1);
            while (port_slots_[slot] >= 0) slot = (slot + 1) & (kHashSlots - 1);
            port_slots_[slot] = static_cast<int16_t>(pid);
        }
        axis_rate_[0] = axis_rate(-1, 0);  // west
        axis_rate_[1] = axis_rate(1, 0);   // east
        axis_rate_[2] = axis_rate(0, -1);  // south
        axis_rate_[3] = axis_rate(0, 1);   // north
        prepare_endpoint_candidates();
    }

    bool parse_endpoint(Slice text, Endpoint& out) const {
        trim(text);
        if (text.size < 9 || std::memcmp(text.data, "SRB_", 4) != 0) return false;
        std::size_t pos = 4;
        int x = 0, y = 0;
        if (!parse_nonnegative(text, pos, x) || pos >= text.size || text.data[pos++] != '_') return false;
        if (!parse_nonnegative(text, pos, y) || pos >= text.size || text.data[pos++] != '/') return false;
        if (x < 0 || x >= 120 || y < 0 || y >= 550 || pos >= text.size) return false;
        const int pid = find_port({text.data + pos, text.size - pos});
        if (pid < 0) return false;
        out = Endpoint{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<uint16_t>(pid)};
        return true;
    }

    bool predict_spec(Slice from, Slice to, uint32_t& result) const {
        Endpoint source, target;
        if (!parse_endpoint(from, source) || !parse_endpoint(to, target)) return false;
        result = predict(source, target);
        return true;
    }

    uint32_t predict(const Endpoint& source, const Endpoint& target) const {
        if (source.x == target.x && source.y == target.y && source.port == target.port) return 0;
        const int dx = int(target.x) - source.x;
        const int dy = int(target.y) - source.y;
        const int source_input = prpr_data::kPortToInput[source.port];
        if (dx == 0 && dy == 0 && source_input >= 0 && prpr_data::kPortToInput[target.port] < 0) {
            uint16_t best = std::numeric_limits<uint16_t>::max();
            for (int i = prpr_data::kDirectOffset[source_input]; i < prpr_data::kDirectOffset[source_input + 1]; ++i) {
                const auto& arc = prpr_data::kDirectArcs[i];
                if (arc.id == target.port) best = std::min(best, arc.cost);
            }
            if (best != std::numeric_limits<uint16_t>::max()) return best;
        }

        const int wanted = vector_bin(dx, dy);
        const auto& sources = source_candidates_[source.port][wanted];
        const auto& targets = target_candidates_[target.port];
        Candidate best;
        for (int si = 0; si < sources.count; ++si) {
            const auto& seed = sources.rows[si];
            for (int ti = 0; ti < targets.count; ++ti) {
                const auto& exit = targets.rows[ti];
                Candidate candidate = travel(seed.state, exit.state, dx - seed.dx, dy - seed.dy);
                candidate.delay += seed.cost + exit.cost;
                keep_best(best, candidate);
            }
        }
        if (!std::isfinite(best.delay)) {
            best.delay = residual_cost(dx, dy);
            best.remainder = std::max(std::abs(dx), std::abs(dy));
        }
        best.delay += std::abs(int(prpr_data::kXGapPrefix[target.x]) - prpr_data::kXGapPrefix[source.x]);
        best.delay += std::abs(int(prpr_data::kYGapPrefix[target.y]) - prpr_data::kYGapPrefix[source.y]);
        if (!(best.delay > 0.0)) return 0;
        const double rounded = std::floor(best.delay + 0.5);
        return rounded >= std::numeric_limits<uint32_t>::max()
                   ? std::numeric_limits<uint32_t>::max()
                   : static_cast<uint32_t>(rounded);
    }

private:
    static constexpr int kHashSlots = 2048;
    struct SourceCandidate { int16_t state, dx, dy; uint16_t cost; };
    struct TargetCandidate { int16_t state; uint16_t cost; };
    template <typename T> struct TwoRows { std::array<T, 2> rows{}; uint8_t count = 0; };

    int16_t port_slots_[kHashSlots]{};
    uint16_t port_lengths_[prpr_data::kPortCount]{};
    const char* port_names_[prpr_data::kPortCount]{};
    double axis_rate_[4]{};
    std::array<std::array<TwoRows<SourceCandidate>, 16>, prpr_data::kPortCount> source_candidates_{};
    std::array<TwoRows<TargetCandidate>, prpr_data::kPortCount> target_candidates_{};

    static uint32_t hash_port(Slice name) {
        uint32_t value = 2166136261u;
        for (std::size_t i = 0; i < name.size; ++i)
            value = (value ^ static_cast<unsigned char>(name.data[i])) * 16777619u;
        return value;
    }

    static void trim(Slice& text) {
        while (text.size && (text.data[0] == ' ' || text.data[0] == '\t' || text.data[0] == '\r')) {
            ++text.data;
            --text.size;
        }
        while (text.size) {
            const char c = text.data[text.size - 1];
            if (c != ' ' && c != '\t' && c != '\r') break;
            --text.size;
        }
    }

    static bool parse_nonnegative(Slice text, std::size_t& pos, int& value) {
        if (pos >= text.size || text.data[pos] < '0' || text.data[pos] > '9') return false;
        int result = 0;
        while (pos < text.size && text.data[pos] >= '0' && text.data[pos] <= '9') {
            result = result * 10 + text.data[pos++] - '0';
            if (result > 100000) return false;
        }
        value = result;
        return true;
    }

    int find_port(Slice name) const {
        uint32_t slot = hash_port(name) & (kHashSlots - 1);
        for (int probe = 0; probe < kHashSlots; ++probe) {
            const int pid = port_slots_[slot];
            if (pid < 0) return -1;
            if (port_lengths_[pid] == name.size &&
                std::memcmp(port_names_[pid], name.data, name.size) == 0)
                return pid;
            slot = (slot + 1) & (kHashSlots - 1);
        }
        return -1;
    }

    static int vector_bin(int dx, int dy) {
        if (dx == 0 && dy == 0) return 0;
        constexpr double pi = 3.14159265358979323846;
        int result = static_cast<int>(std::floor((std::atan2(double(dy), double(dx)) + pi) * 16.0 / (2.0 * pi) + 0.5));
        return result & 15;
    }

    static double alignment_penalty(int dx, int dy, int wanted) {
        if (dx == 0 && dy == 0) return 50.0;
        int delta = std::abs(vector_bin(dx, dy) - wanted);
        delta = std::min(delta, 16 - delta);
        return 12.0 * delta;
    }

    double axis_rate(int sign_x, int sign_y) const {
        double best = std::numeric_limits<double>::infinity();
        for (const auto& primitive : prpr_data::kPrimitives) {
            const bool wanted = sign_x ? (primitive.dx * sign_x > 0 && primitive.dy == 0)
                                       : (primitive.dy * sign_y > 0 && primitive.dx == 0);
            if (wanted) best = std::min(best, double(primitive.cost) / (std::abs(primitive.dx) + std::abs(primitive.dy)));
        }
        return best;
    }

    void prepare_endpoint_candidates() {
        for (int pid = 0; pid < prpr_data::kPortCount; ++pid) {
            for (int wanted = 0; wanted < 16; ++wanted) {
                std::vector<SourceCandidate> candidates;
                const int iid = prpr_data::kPortToInput[pid];
                if (iid >= 0) {
                    const int state = prpr_data::kInputToState[iid];
                    if (state >= 0) {
                        candidates.push_back({static_cast<int16_t>(state), 0, 0, 0});
                    } else {
                        for (int i = prpr_data::kDirectOffset[iid]; i < prpr_data::kDirectOffset[iid + 1]; ++i) {
                            const auto& arc = prpr_data::kDirectArcs[i];
                            const auto& net = prpr_data::kOutputNet[arc.id];
                            if (net.state >= 0) candidates.push_back({net.state, net.dx, net.dy, arc.cost});
                        }
                    }
                } else {
                    const auto& net = prpr_data::kOutputNet[pid];
                    if (net.state >= 0) candidates.push_back({net.state, net.dx, net.dy, 0});
                }
                std::sort(candidates.begin(), candidates.end(), [wanted](const auto& a, const auto& b) {
                    const auto ka = std::make_tuple(a.cost + alignment_penalty(a.dx, a.dy, wanted), a.state, a.dx, a.dy, a.cost);
                    const auto kb = std::make_tuple(b.cost + alignment_penalty(b.dx, b.dy, wanted), b.state, b.dx, b.dy, b.cost);
                    return ka < kb;
                });
                auto& output = source_candidates_[pid][wanted];
                output.count = static_cast<uint8_t>(std::min<std::size_t>(1, candidates.size()));
                for (int i = 0; i < output.count; ++i) output.rows[i] = candidates[i];
            }

            std::vector<TargetCandidate> candidates;
            const int iid = prpr_data::kPortToInput[pid];
            if (iid >= 0) {
                const int state = prpr_data::kInputToState[iid];
                if (state >= 0) candidates.push_back({static_cast<int16_t>(state), 0});
            } else {
                for (int i = prpr_data::kTargetOffset[pid]; i < prpr_data::kTargetOffset[pid + 1]; ++i) {
                    const auto& arc = prpr_data::kTargetArcs[i];
                    candidates.push_back({static_cast<int16_t>(arc.id), arc.cost});
                }
                std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
                    return std::tie(a.cost, a.state) < std::tie(b.cost, b.state);
                });
            }
            auto& output = target_candidates_[pid];
            output.count = static_cast<uint8_t>(std::min<std::size_t>(1, candidates.size()));
            for (int i = 0; i < output.count; ++i) output.rows[i] = candidates[i];
        }
    }

    static const prpr_data::Connector& connector(int source_class, int target_class) {
        return prpr_data::kConnector[source_class * 16 + target_class];
    }

    double residual_cost(int dx, int dy) const {
        const double x_rate = axis_rate_[dx >= 0 ? 1 : 0];
        const double y_rate = axis_rate_[dy >= 0 ? 3 : 2];
        return std::abs(dx) * x_rate + std::abs(dy) * y_rate;
    }

    static void keep_best(Candidate& best, const Candidate& candidate) {
        if (candidate.delay < best.delay || (candidate.delay == best.delay && candidate.remainder < best.remainder)) best = candidate;
    }

    static std::array<int, 4> count_options(double value) {
        return {0, std::max(0, int(std::floor(value))), std::max(0, int(std::ceil(value))), std::max(0, int(std::round(value)))};
    }

    const prpr_data::Primitive& runtime_primitive(int direction, int dx, int dy) const {
        const double wanted = std::min(120.0, std::max(12.0, std::hypot(double(dx), double(dy))));
        const prpr_data::Primitive* best = nullptr;
        auto best_key = std::make_tuple(std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), 0xffff);
        for (const auto& primitive : prpr_data::kPrimitives) {
            if (primitive.direction != direction) continue;
            const double magnitude = std::hypot(double(primitive.dx), double(primitive.dy));
            const auto key = std::make_tuple(std::abs(magnitude - wanted), primitive.cost / std::max(magnitude, 1.0), int(primitive.id));
            if (!best || key < best_key) {
                best = &primitive;
                best_key = key;
            }
        }
        return *best;
    }

    Candidate single_candidate(int source_class, int target_class, int dx, int dy, const prpr_data::Primitive& primitive) const {
        const auto& pre = connector(source_class, primitive.entry_class);
        const auto& post = connector(primitive.entry_class, target_class);
        const int vx = dx - pre.dx - post.dx;
        const int vy = dy - pre.dy - post.dy;
        const int denominator = primitive.dx * primitive.dx + primitive.dy * primitive.dy;
        const double projection = double(vx * primitive.dx + vy * primitive.dy) / std::max(denominator, 1);
        Candidate best;
        for (const int count : count_options(projection)) {
            const int rx = vx - count * primitive.dx;
            const int ry = vy - count * primitive.dy;
            keep_best(best, {double(pre.cost + post.cost + count * primitive.cost) + residual_cost(rx, ry), std::max(std::abs(rx), std::abs(ry))});
        }
        return best;
    }

    Candidate pair_candidate(int source_class, int target_class, int dx, int dy,
                             const prpr_data::Primitive& first, const prpr_data::Primitive& second) const {
        const auto& pre = connector(source_class, first.entry_class);
        const auto& middle = connector(first.entry_class, second.entry_class);
        const auto& post = connector(second.entry_class, target_class);
        const int vx = dx - pre.dx - middle.dx - post.dx;
        const int vy = dy - pre.dy - middle.dy - post.dy;
        const int determinant = first.dx * second.dy - first.dy * second.dx;
        Candidate best;
        if (determinant == 0) return best;
        const double real1 = double(vx * second.dy - vy * second.dx) / determinant;
        const double real2 = double(first.dx * vy - first.dy * vx) / determinant;
        if (real1 < -0.75 || real2 < -0.75) return best;
        for (const int n1 : count_options(real1)) {
            for (const int n2 : count_options(real2)) {
                const int rx = vx - n1 * first.dx - n2 * second.dx;
                const int ry = vy - n1 * first.dy - n2 * second.dy;
                keep_best(best, {
                    double(pre.cost + middle.cost + post.cost + n1 * first.cost + n2 * second.cost) + residual_cost(rx, ry),
                    std::max(std::abs(rx), std::abs(ry))
                });
            }
        }
        return best;
    }

    Candidate travel(int source_state, int target_state, int dx, int dy) const {
        const int source_class = prpr_data::kStateClass[source_state];
        const int target_class = prpr_data::kStateClass[target_state];
        const auto& direct = connector(source_class, target_class);
        Candidate best{direct.cost + residual_cost(dx - direct.dx, dy - direct.dy),
                       std::max(std::abs(dx - direct.dx), std::abs(dy - direct.dy))};
        const int wanted = vector_bin(dx, dy);
        for (int offset = -1; offset <= 1; ++offset) {
            const auto& primitive = runtime_primitive((wanted + offset) & 15, dx, dy);
            keep_best(best, single_candidate(source_class, target_class, dx, dy, primitive));
        }
        const int horizontal = dx >= 0 ? 8 : 0;
        const int vertical = dy >= 0 ? 12 : 4;
        const std::array<std::pair<int, int>, 3> pairs{{
            {((wanted - 1) & 15), wanted},
            {wanted, ((wanted + 1) & 15)},
            {horizontal, vertical},
        }};
        for (int index = 0; index < 3; ++index) {
            if (index == 2 && (pairs[index] == pairs[0] || pairs[index] == pairs[1])) continue;
            const auto& first = runtime_primitive(pairs[index].first, dx, dy);
            const auto& second = runtime_primitive(pairs[index].second, dx, dy);
            keep_best(best, pair_candidate(source_class, target_class, dx, dy, first, second));
        }
        return best;
    }
};

}  // namespace prpr
