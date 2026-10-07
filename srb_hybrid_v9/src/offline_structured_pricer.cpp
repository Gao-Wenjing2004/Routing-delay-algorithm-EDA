#include "srb_core_v9.hpp"
#include "p10_periodic_portal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr int kStates = 160;
#ifndef P8_AXIS_RADIUS
#define P8_AXIS_RADIUS 32
#endif
constexpr int kRadius = P8_AXIS_RADIUS;
constexpr int kWidth = 2 * kRadius + 1;
constexpr uint16_t kInf16 = 65535;
constexpr uint32_t kBig = 1000000000u;

struct Options {
    std::string model;
    std::string axis;
    std::string candidates;
    std::string output;
    std::string runtime_table;
    std::string selector_table;
    int state_beam = 32;
    int edge_beam = 160;
    int candidate_limit = 32;
    int passes = 1;
    bool warm_cache = false;
    bool block_portals = false;
    int block_portal_band = 1;
    std::string portal_decomposition;
    std::string portal_states;
    std::string portal_up_closure;
    std::string portal_down_closure;
    int portal_max_beam = 32;
};

struct OutputNet { int route = -1, dx = 0, dy = 0; };
struct Source { int state = -1, dx = 0, dy = 0, cost = 0; };
struct Gap { bool vertical = false; int site = 0, delay = 0; };
struct Rect {
    int left = 0, right = 0, lower = 0, upper = 0;
    bool vertical_crossable = false, horizontal_crossable = false;
    int vertical_delay = 0, horizontal_delay = 0;
};

struct TemplateCandidate {
    std::string pattern;
    std::vector<char> axes;
    int h_trunk = -1;
    int v_trunk = -1;
    std::vector<int> local;
    bool mutation = false;
};

struct Request {
    std::string from, to;
    int golden = 0;
    std::vector<TemplateCandidate> candidates;
};

struct Endpoint { int x = 0, y = 0, port = -1; };

std::string value(int& index, int argc, char** argv, const std::string& option) {
    if (++index >= argc) throw std::runtime_error("missing value for " + option);
    return argv[index];
}

Options parse_options(int argc, char** argv) {
    Options result;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--model") result.model = value(index, argc, argv, argument);
        else if (argument == "--axis") result.axis = value(index, argc, argv, argument);
        else if (argument == "--candidates") result.candidates = value(index, argc, argv, argument);
        else if (argument == "--output") result.output = value(index, argc, argv, argument);
        else if (argument == "--runtime-table") result.runtime_table = value(index, argc, argv, argument);
        else if (argument == "--selector-table") result.selector_table = value(index, argc, argv, argument);
        else if (argument == "--state-beam") result.state_beam = std::stoi(value(index, argc, argv, argument));
        else if (argument == "--edge-beam") result.edge_beam = std::stoi(value(index, argc, argv, argument));
        else if (argument == "--candidate-limit") result.candidate_limit = std::stoi(value(index, argc, argv, argument));
        else if (argument == "--passes") result.passes = std::stoi(value(index, argc, argv, argument));
        else if (argument == "--warm-cache") result.warm_cache = true;
        else if (argument == "--block-portals") result.block_portals = true;
        else if (argument == "--block-portal-band") {
            result.block_portals = true;
            result.block_portal_band = std::stoi(value(index, argc, argv, argument));
        }
        else if (argument == "--portal-decomposition")
            result.portal_decomposition = value(index, argc, argv, argument);
        else if (argument == "--portal-states")
            result.portal_states = value(index, argc, argv, argument);
        else if (argument == "--portal-up-closure")
            result.portal_up_closure = value(index, argc, argv, argument);
        else if (argument == "--portal-down-closure")
            result.portal_down_closure = value(index, argc, argv, argument);
        else if (argument == "--portal-max-beam")
            result.portal_max_beam = std::stoi(value(index, argc, argv, argument));
        else throw std::runtime_error("unknown argument: " + argument);
    }
    const bool portal_mode = !result.portal_decomposition.empty();
    if (result.model.empty() || result.axis.empty() || result.output.empty() ||
        (!portal_mode && result.candidates.empty()))
        throw std::runtime_error(
            "usage: offline_structured_pricer --model prpr_model.json --axis axis.bin "
            "--candidates candidates.csv --output audit.csv [--candidate-limit 32] "
            "[--state-beam 32] [--edge-beam 160] [--runtime-table table.bin] "
            "[--selector-table selector.bin] [--passes 1] [--warm-cache]; or "
            "--portal-decomposition decomposition.csv --portal-states states.csv "
            "--portal-up-closure up.bin --portal-down-closure down.bin "
            "--runtime-table table.bin --output audit.csv");
    if (portal_mode && (result.portal_states.empty() || result.portal_up_closure.empty() ||
                        result.portal_down_closure.empty() || result.runtime_table.empty()))
        throw std::runtime_error("Portal evaluation requires states, both closures, and runtime table");
    if (result.state_beam <= 0 || result.state_beam > kStates)
        throw std::runtime_error("state beam must be in [1,160]");
    if (result.edge_beam <= 0 || result.edge_beam > kStates)
        throw std::runtime_error("edge beam must be in [1,160]");
    if (result.candidate_limit <= 0)
        throw std::runtime_error("candidate limit must be positive");
    if (result.passes <= 0)
        throw std::runtime_error("passes must be positive");
    if (result.block_portal_band <= 0 || result.block_portal_band > 12)
        throw std::runtime_error("block portal band must be in [1,12]");
    if (result.portal_max_beam <= 0 || result.portal_max_beam > 1920)
        throw std::runtime_error("portal max beam must be in [1,1920]");
    return result;
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> result;
    size_t begin = 0;
    while (true) {
        const size_t end = text.find(separator, begin);
        result.push_back(text.substr(begin, end == std::string::npos ? end : end - begin));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return result;
}

class Pricer {
public:
    explicit Pricer(const Options& options)
        : state_beam_(options.state_beam), edge_beam_(options.edge_beam),
          candidate_limit_(options.candidate_limit), block_portals_(options.block_portals),
          block_portal_band_(options.block_portal_band) {
        load_model(options.model);
        load_axis(options.axis);
        build_top_edges();
    }

    Endpoint endpoint(const std::string& specification) const {
        const size_t slash = specification.find('/');
        if (slash == std::string::npos) throw std::runtime_error("bad endpoint: " + specification);
        const std::string instance = specification.substr(0, slash);
        if (instance.rfind("SRB_", 0) != 0) throw std::runtime_error("bad instance: " + instance);
        const size_t underscore = instance.find('_', 4);
        const auto port = port_ids_.find(specification.substr(slash + 1));
        if (underscore == std::string::npos || port == port_ids_.end())
            throw std::runtime_error("unknown endpoint: " + specification);
        return Endpoint{
            std::stoi(instance.substr(4, underscore - 4)),
            std::stoi(instance.substr(underscore + 1)), port->second};
    }

    uint32_t query(
        const std::string& from_spec, const std::string& to_spec,
        const std::vector<TemplateCandidate>& candidates,
        uint64_t& transition_count, int& valid_candidates) const {
        const Endpoint from = endpoint(from_spec);
        const Endpoint to = endpoint(to_spec);
        const int total_dx = to.x - from.x;
        const int total_dy = to.y - from.y;
        const std::vector<Source> sources = source_candidates(from.port, from.x, from.y);
        // A periodic Portal endpoint is itself a Routing Input.  The original
        // competition requests normally terminate at an Output, so the P6
        // pricer only populated target_arcs_ for those ports.  Allowing the
        // DP to stop directly in the input's routing state is the exact
        // zero-cost terminal connector needed by P10/P11; it does not alter
        // ordinary Output endpoint queries.
        std::vector<std::pair<int, int>> direct_target;
        const std::vector<std::pair<int, int>>* targets = &target_arcs_[to.port];
        const int target_iid = port_to_input_[to.port];
        if (target_iid >= 0 && input_to_state_[target_iid] >= 0) {
            direct_target.push_back({input_to_state_[target_iid], 0});
            targets = &direct_target;
        }
        uint32_t best = from_spec == to_spec ? 0u : kBig;
        const int iid = port_to_input_[from.port];
        if (from.x == to.x && from.y == to.y && iid >= 0) {
            for (const auto& arc : direct_arcs_[iid])
                if (arc.first == to.port) best = std::min(best, static_cast<uint32_t>(arc.second));
        }
        const int candidate_count = std::min<int>(candidate_limit_, candidates.size());
        for (int candidate_index = 0; candidate_index < candidate_count; ++candidate_index) {
            const TemplateCandidate& candidate = candidates[candidate_index];
            for (const Source& source : sources) {
                std::vector<int> deltas;
                if (!instantiate(candidate, total_dx - source.dx, total_dy - source.dy, deltas))
                    continue;
                const uint32_t priced = price_candidate(
                    from.x, from.y, total_dx, total_dy, source, *targets,
                    candidate.axes, deltas, transition_count);
                if (priced < kBig) {
                    ++valid_candidates;
                    best = std::min(best, priced);
                }
            }
        }
        if (block_portals_) {
            for (const Source& source : sources) {
                const int source_x = from.x + source.dx;
                const int source_y = from.y + source.dy;
                for (const Rect& block : blocks_) {
                    if (block.vertical_crossable ||
                        source_x < block.left || source_x > block.right ||
                        to.x < block.left || to.x > block.right)
                        continue;
                    const int min_y = std::min(source_y, to.y);
                    const int max_y = std::max(source_y, to.y);
                    if (max_y < block.lower || min_y > block.upper) continue;
                    std::array<int, 24> portal_xs{};
                    int portal_count = 0;
                    for (int offset = 1; offset <= block_portal_band_; ++offset) {
                        portal_xs[portal_count++] = block.left - offset;
                        portal_xs[portal_count++] = block.right + offset;
                    }
                    for (int portal_index = 0; portal_index < portal_count; ++portal_index) {
                        const int portal_x = portal_xs[portal_index];
                        if (portal_x < 0 || portal_x >= width_) continue;
                        std::vector<char> axes{'H', 'V', 'H'};
                        std::vector<int> deltas{
                            portal_x - source_x, to.y - source_y, to.x - portal_x};
                        const char entry_axis = source.dx ? 'H' : source.dy ? 'V' : 0;
                        if (entry_axis == 'V') {
                            axes.insert(axes.begin(), 'V');
                            deltas.insert(deltas.begin(), 0);
                        }
                        const uint32_t priced = price_candidate(
                            from.x, from.y, total_dx, total_dy, source, *targets,
                            axes, deltas, transition_count);
                        if (priced < kBig) {
                            ++valid_candidates;
                            best = std::min(best, priced);
                        }
                    }
                    // All periodic Blocks share the same two vertical corridors;
                    // adding the same portal pair for every intersected Block is redundant.
                    break;
                }
            }
        }
        return best;
    }

private:
    struct EdgeChoice { uint16_t target = 0, cost = kInf16; };
    int width_ = 0, height_ = 0, state_beam_ = 32, edge_beam_ = kStates;
    int candidate_limit_ = 32;
    bool block_portals_ = false;
    int block_portal_band_ = 1;
    std::unordered_map<std::string, int> port_ids_;
    std::vector<int> port_to_input_, input_to_state_;
    std::vector<OutputNet> output_nets_;
    std::vector<std::vector<std::pair<int, int>>> direct_arcs_, target_arcs_;
    std::vector<Gap> gaps_;
    std::vector<Rect> blocks_;
    std::vector<uint16_t> axis_;
    std::vector<EdgeChoice> top_edges_;
    struct SparseAxisSection {
        std::vector<uint32_t> offsets;
        std::vector<uint8_t> targets;
        std::vector<uint16_t> costs;
        int distance_count = 0;
    };
    bool sparse_axis_ = false;
    int sparse_cutoff_ = 0, horizontal_max_ = kRadius, vertical_max_ = kRadius;
    int sparse_prefix_width_ = 0;
    std::vector<uint16_t> sparse_prefix_;
    std::array<SparseAxisSection, 4> sparse_sections_;

    static int integer(const Json& value) {
        if (value.type != Json::Type::Int) throw std::runtime_error("expected JSON integer");
        return static_cast<int>(value.i);
    }

    void load_model(const std::string& path) {
        const Json model = load_json(path);
        width_ = iget(model, "width");
        height_ = iget(model, "height");
        const Json& names = model.at("port_names");
        for (size_t index = 0; index < names.a.size(); ++index)
            port_ids_[names.a[index].s] = static_cast<int>(index);
        for (const Json& item : model.at("port_to_input").a) port_to_input_.push_back(integer(item));
        for (const Json& item : model.at("input_to_state").a) input_to_state_.push_back(integer(item));
        for (const Json& item : model.at("output_nets").a)
            output_nets_.push_back(OutputNet{integer(item.a[0]), integer(item.a[1]), integer(item.a[2])});
        for (const Json& rows : model.at("direct_arcs").a) {
            direct_arcs_.emplace_back();
            for (const Json& item : rows.a)
                direct_arcs_.back().push_back({integer(item.a[0]), integer(item.a[1])});
        }
        for (const Json& rows : model.at("target_arcs").a) {
            target_arcs_.emplace_back();
            for (const Json& item : rows.a)
                target_arcs_.back().push_back({integer(item.a[0]), integer(item.a[1])});
        }
        for (const Json& item : model.at("gaps").a)
            gaps_.push_back(Gap{
                sget(item, "direction") == "vertical", iget(item, "site"), iget(item, "delay")});
        for (const Json& item : model.at("blocks").a)
            blocks_.push_back(Rect{
                iget(item, "left"), iget(item, "right"), iget(item, "lower"), iget(item, "upper"),
                bget(item, "vertical crossable"), bget(item, "horizontal crossable"),
                iget(item, "vertical cross delay"), iget(item, "horizontal cross delay")});
    }

    void load_axis(const std::string& path) {
        std::ifstream probe(path, std::ios::binary);
        std::array<char, 4> magic{};
        if (!probe || !probe.read(magic.data(), magic.size()))
            throw std::runtime_error("cannot read axis table: " + path);
        if (std::memcmp(magic.data(), "AXS9", 4) == 0) {
            auto read_u16 = [&]() {
                uint16_t value = 0;
                if (!probe.read(reinterpret_cast<char*>(&value), sizeof(value)))
                    throw std::runtime_error("truncated sparse axis header");
                return value;
            };
            auto read_u64 = [&]() {
                uint64_t value = 0;
                if (!probe.read(reinterpret_cast<char*>(&value), sizeof(value)))
                    throw std::runtime_error("truncated sparse axis section");
                return value;
            };
            const uint16_t version = read_u16();
            const uint16_t states = read_u16();
            sparse_cutoff_ = read_u16();
            horizontal_max_ = read_u16();
            vertical_max_ = read_u16();
            if (version != 1 || states != kStates || sparse_cutoff_ <= 0)
                throw std::runtime_error("unsupported sparse axis table");
            sparse_prefix_width_ = 2 * sparse_cutoff_ - 1;
            const uint64_t dense_count = read_u64();
            const uint64_t expected_dense =
                2ull * kStates * sparse_prefix_width_ * kStates;
            if (dense_count != expected_dense)
                throw std::runtime_error("bad sparse axis prefix shape");
            sparse_prefix_.resize(static_cast<size_t>(dense_count));
            if (!probe.read(reinterpret_cast<char*>(sparse_prefix_.data()),
                            static_cast<std::streamsize>(dense_count * sizeof(uint16_t))))
                throw std::runtime_error("truncated sparse axis prefix");
            for (int section_index = 0; section_index < 4; ++section_index) {
                const uint64_t offset_count = read_u64();
                const uint64_t target_count = read_u64();
                const uint64_t cost_count = read_u64();
                SparseAxisSection& section = sparse_sections_[section_index];
                if (offset_count != kStates + 1)
                    throw std::runtime_error("bad sparse axis offsets");
                section.offsets.resize(static_cast<size_t>(offset_count));
                section.targets.resize(static_cast<size_t>(target_count));
                section.costs.resize(static_cast<size_t>(cost_count));
                if (!probe.read(reinterpret_cast<char*>(section.offsets.data()),
                                static_cast<std::streamsize>(offset_count * sizeof(uint32_t))) ||
                    !probe.read(reinterpret_cast<char*>(section.targets.data()),
                                static_cast<std::streamsize>(target_count * sizeof(uint8_t))) ||
                    !probe.read(reinterpret_cast<char*>(section.costs.data()),
                                static_cast<std::streamsize>(cost_count * sizeof(uint16_t))))
                    throw std::runtime_error("truncated sparse axis data");
                const int axis = section_index / 2;
                const int maximum = axis == 0 ? horizontal_max_ : vertical_max_;
                section.distance_count = maximum - sparse_cutoff_ + 1;
                if (section.offsets.back() != target_count ||
                    cost_count != target_count * static_cast<uint64_t>(section.distance_count))
                    throw std::runtime_error("bad sparse axis section shape");
            }
            sparse_axis_ = true;
            return;
        }
        probe.close();
        const size_t count = 2ull * kStates * kWidth * kStates;
        axis_.resize(count);
        std::ifstream stream(path, std::ios::binary);
        if (!stream || !stream.read(reinterpret_cast<char*>(axis_.data()),
                                    static_cast<std::streamsize>(count * sizeof(uint16_t))))
            throw std::runtime_error("cannot read axis table: " + path);
    }

    const uint16_t* table(int axis, int state, int delta) const {
        if (sparse_axis_) {
            if (std::abs(delta) >= sparse_cutoff_)
                throw std::runtime_error("sparse long transfer has no dense row");
            return sparse_prefix_.data() +
                (static_cast<size_t>(axis) * kStates * sparse_prefix_width_ +
                 static_cast<size_t>(state) * sparse_prefix_width_ +
                 delta + sparse_cutoff_ - 1) * kStates;
        }
        const size_t one_axis = static_cast<size_t>(kStates) * kWidth * kStates;
        return axis_.data() + axis * one_axis +
            (static_cast<size_t>(state) * kWidth + delta + kRadius) * kStates;
    }

    uint16_t axis_cost(int axis, int state, int delta, int target) const {
        if (!sparse_axis_ || std::abs(delta) < sparse_cutoff_)
            return table(axis, state, delta)[target];
        const int maximum = axis == 0 ? horizontal_max_ : vertical_max_;
        if (std::abs(delta) > maximum) return kInf16;
        const int section_index = axis * 2 + (delta > 0 ? 1 : 0);
        const SparseAxisSection& section = sparse_sections_[section_index];
        const uint32_t begin = section.offsets[state], end = section.offsets[state + 1];
        for (uint32_t index = begin; index < end; ++index) {
            if (section.targets[index] != target) continue;
            return section.costs[
                static_cast<size_t>(index) * section.distance_count +
                std::abs(delta) - sparse_cutoff_];
        }
        return kInf16;
    }

    void build_top_edges() {
        if (sparse_axis_ || edge_beam_ == kStates) return;
        top_edges_.resize(static_cast<size_t>(2) * kStates * kWidth * edge_beam_);
        for (int axis = 0; axis < 2; ++axis)
            for (int source = 0; source < kStates; ++source)
                for (int delta = -kRadius; delta <= kRadius; ++delta) {
                    std::array<EdgeChoice, kStates> choices;
                    const uint16_t* costs = table(axis, source, delta);
                    for (int target = 0; target < kStates; ++target)
                        choices[target] = EdgeChoice{static_cast<uint16_t>(target), costs[target]};
                    std::partial_sort(
                        choices.begin(), choices.begin() + edge_beam_, choices.end(),
                        [](const EdgeChoice& left, const EdgeChoice& right) {
                            return std::pair<uint16_t, uint16_t>{left.cost, left.target} <
                                   std::pair<uint16_t, uint16_t>{right.cost, right.target};
                        });
                    const size_t base = (((static_cast<size_t>(axis) * kStates + source) * kWidth +
                                          delta + kRadius) * edge_beam_);
                    std::copy_n(choices.begin(), edge_beam_, top_edges_.begin() + base);
                }
    }

    const EdgeChoice* top_edges(int axis, int state, int delta) const {
        const size_t base = (((static_cast<size_t>(axis) * kStates + state) * kWidth +
                              delta + kRadius) * edge_beam_);
        return top_edges_.data() + base;
    }

    const Rect* block_at(int x, int y) const {
        for (const Rect& block : blocks_)
            if (x >= block.left && x <= block.right &&
                y >= block.lower && y <= block.upper) return &block;
        return nullptr;
    }

    uint32_t line_penalty(int x, int y, char axis, int delta) const {
        const int nx = x + (axis == 'H' ? delta : 0);
        const int ny = y + (axis == 'V' ? delta : 0);
        const int min_x = std::min(x, nx), max_x = std::max(x, nx);
        const int min_y = std::min(y, ny), max_y = std::max(y, ny);
        uint32_t result = 0;
        for (const Gap& gap : gaps_) {
            if (axis == 'H' && gap.vertical && min_x <= gap.site && gap.site < max_x)
                result += gap.delay;
            else if (axis == 'V' && !gap.vertical && min_y <= gap.site && gap.site < max_y)
                result += gap.delay;
        }
        return result;
    }

    // Advance a single official Net rule.  Crossable Block cells do not
    // consume Net displacement and charge the Block delay once, exactly like
    // the full Dijkstra graph.
    bool advance_routing_delta(
        int x, int y, char axis, int routing_delta,
        int& actual_dx, int& actual_dy, uint32_t& extra) const {
        const int step = routing_delta >= 0 ? 1 : -1;
        const int need = std::abs(routing_delta);
        const int sx = x, sy = y;
        int counted = 0;
        std::array<const Rect*, 8> charged{};
        int charged_count = 0;
        extra = 0;
        while (counted < need) {
            if (axis == 'H') x += step; else y += step;
            if (x < 0 || x >= width_ || y < 0 || y >= height_) return false;
            const Rect* block = block_at(x, y);
            if (!block) {
                ++counted;
                continue;
            }
            const bool crossable = axis == 'H'
                ? block->horizontal_crossable : block->vertical_crossable;
            if (!crossable) {
                ++counted;
                continue;
            }
            bool seen = false;
            for (int index = 0; index < charged_count; ++index)
                seen = seen || charged[index] == block;
            if (!seen) {
                charged[charged_count++] = block;
                extra += axis == 'H' ? block->horizontal_delay : block->vertical_delay;
            }
        }
        if (block_at(x, y)) return false;
        actual_dx = x - sx;
        actual_dy = y - sy;
        extra += line_penalty(sx, sy, axis, axis == 'H' ? actual_dx : actual_dy);
        return true;
    }

    std::vector<Source> source_candidates(int port, int x, int y) const {
        std::vector<Source> result;
        const int iid = port_to_input_[port];
        if (iid >= 0) {
            const int route = input_to_state_[iid];
            if (route >= 0) return {Source{route, 0, 0, 0}};
            for (const auto& arc : direct_arcs_[iid]) {
                const OutputNet& net = output_nets_[arc.first];
                int dx = 0, dy = 0;
                uint32_t extra = 0;
                const char axis = net.dx ? 'H' : 'V';
                const int delta = net.dx ? net.dx : net.dy;
                if (net.route >= 0 && advance_routing_delta(
                        x, y, axis, delta, dx, dy, extra))
                    result.push_back(Source{
                        net.route, dx, dy, arc.second + static_cast<int>(extra)});
            }
            return result;
        }
        const OutputNet& net = output_nets_[port];
        int dx = 0, dy = 0;
        uint32_t extra = 0;
        const char axis = net.dx ? 'H' : 'V';
        const int delta = net.dx ? net.dx : net.dy;
        if (net.route >= 0 && advance_routing_delta(x, y, axis, delta, dx, dy, extra))
            result.push_back(Source{net.route, dx, dy, static_cast<int>(extra)});
        return result;
    }

    bool segment_extra(
        int x, int y, char axis, int delta,
        uint32_t& extra, int& routing_delta) const {
        const int nx = x + (axis == 'H' ? delta : 0);
        const int ny = y + (axis == 'V' ? delta : 0);
        if (nx < 0 || nx >= width_ || ny < 0 || ny >= height_) return false;
        const int step = delta >= 0 ? 1 : -1;
        int cx = x, cy = y, counted = 0;
        std::array<const Rect*, 8> charged{};
        int charged_count = 0;
        extra = 0;
        for (int moved = 0; moved < std::abs(delta); ++moved) {
            if (axis == 'H') cx += step; else cy += step;
            const Rect* block = block_at(cx, cy);
            if (!block) {
                ++counted;
                continue;
            }
            const bool crossable = axis == 'H'
                ? block->horizontal_crossable : block->vertical_crossable;
            if (!crossable) return false;
            bool seen = false;
            for (int index = 0; index < charged_count; ++index)
                seen = seen || charged[index] == block;
            if (!seen) {
                charged[charged_count++] = block;
                extra += axis == 'H' ? block->horizontal_delay : block->vertical_delay;
            }
        }
        routing_delta = step * counted;
        extra += line_penalty(x, y, axis, delta);
        return true;
    }

    bool instantiate(const TemplateCandidate& candidate, int required_dx, int required_dy,
                     std::vector<int>& values) const {
        values = candidate.local;
        for (const auto axis_required : {std::pair<char, int>{'H', required_dx}, {'V', required_dy}}) {
            const char axis = axis_required.first;
            const int required = axis_required.second;
            std::vector<int> indices;
            for (size_t index = 0; index < candidate.axes.size(); ++index)
                if (candidate.axes[index] == axis) indices.push_back(static_cast<int>(index));
            if (indices.empty()) {
                if (required != 0) return false;
                continue;
            }
            const int trunk = axis == 'H' ? candidate.h_trunk : candidate.v_trunk;
            if (std::find(indices.begin(), indices.end(), trunk) == indices.end()) return false;
            int sum = 0;
            for (int index : indices) if (index != trunk) sum += values[index];
            values[trunk] = required - sum;
        }
        for (size_t index = 0; index < values.size(); ++index) {
            const int maximum = sparse_axis_
                ? (candidate.axes[index] == 'H' ? horizontal_max_ : vertical_max_)
                : kRadius;
            if (values[index] < -maximum || values[index] > maximum) return false;
        }
        return true;
    }

    uint32_t price_candidate(
        int sx, int sy, int total_dx, int total_dy, const Source& source,
        const std::vector<std::pair<int, int>>& targets, const std::vector<char>& axes,
        const std::vector<int>& deltas, uint64_t& transition_count) const {
        const char entry_axis = source.dx ? 'H' : source.dy ? 'V' : 0;
        if (entry_axis && (axes.empty() || axes.front() != entry_axis)) return kBig;
        std::array<uint32_t, kStates> costs, next;
        std::array<int, kStates> active{}, next_active{};
        costs.fill(kBig);
        costs[source.state] = static_cast<uint32_t>(source.cost);
        active[0] = source.state;
        int active_count = 1;
        int x = source.dx, y = source.dy;
        for (size_t run = 0; run < axes.size(); ++run) {
            const char axis_char = axes[run];
            const int delta = deltas[run];
            const bool source_noop = run == 0 && entry_axis == axis_char && delta == 0;
            if (!source_noop) {
                uint32_t extra = 0;
                int routing_delta = 0;
                if (!segment_extra(
                        sx + x, sy + y, axis_char, delta, extra, routing_delta)) return kBig;
                next.fill(kBig);
                const bool final_run = run + 1 == axes.size();
                if (final_run) {
                    uint32_t best = kBig;
                    for (int active_index = 0; active_index < active_count; ++active_index) {
                        const int source_state = active[active_index];
                        transition_count += targets.size();
                        for (const auto& target : targets) {
                            const uint16_t edge = axis_cost(
                                axis_char == 'H' ? 0 : 1,
                                source_state, routing_delta, target.first);
                            if (edge != kInf16)
                                best = std::min(
                                    best, costs[source_state] + edge + extra +
                                        static_cast<uint32_t>(target.second));
                        }
                    }
                    if (axis_char == 'H') x += delta; else y += delta;
                    return x == total_dx && y == total_dy ? best : kBig;
                }
                for (int active_index = 0; active_index < active_count; ++active_index) {
                    const int source_state = active[active_index];
                    const int axis = axis_char == 'H' ? 0 : 1;
                    if (sparse_axis_ && std::abs(routing_delta) >= sparse_cutoff_) {
                        const int section_index = axis * 2 + (routing_delta > 0 ? 1 : 0);
                        const SparseAxisSection& section = sparse_sections_[section_index];
                        const uint32_t begin = section.offsets[source_state];
                        const uint32_t end = section.offsets[source_state + 1];
                        transition_count += end - begin;
                        const int distance_index = std::abs(routing_delta) - sparse_cutoff_;
                        for (uint32_t index = begin; index < end; ++index) {
                            const int target_state = section.targets[index];
                            const uint16_t edge = section.costs[
                                static_cast<size_t>(index) * section.distance_count +
                                distance_index];
                            next[target_state] = std::min(
                                next[target_state], costs[source_state] + edge + extra);
                        }
                    } else if (edge_beam_ == kStates || sparse_axis_) {
                        const uint16_t* edges = table(axis, source_state, routing_delta);
                        transition_count += kStates;
                        for (int target_state = 0; target_state < kStates; ++target_state)
                            if (edges[target_state] != kInf16)
                                next[target_state] = std::min(
                                    next[target_state], costs[source_state] + edges[target_state] + extra);
                    } else {
                        transition_count += edge_beam_;
                        const EdgeChoice* choices = top_edges(axis, source_state, routing_delta);
                        for (int index = 0; index < edge_beam_; ++index) {
                            const EdgeChoice& edge = choices[index];
                            if (edge.cost != kInf16)
                                next[edge.target] = std::min(
                                    next[edge.target], costs[source_state] + edge.cost + extra);
                        }
                    }
                }
                int next_count = 0;
                for (int state = 0; state < kStates; ++state)
                    if (next[state] < kBig) next_active[next_count++] = state;
                if (next_count > state_beam_) {
                    std::nth_element(
                        next_active.begin(), next_active.begin() + state_beam_,
                        next_active.begin() + next_count,
                        [&next](int left, int right) {
                            return std::pair<uint32_t, int>{next[left], left} <
                                   std::pair<uint32_t, int>{next[right], right};
                        });
                    next_count = state_beam_;
                }
                costs = next;
                std::copy_n(next_active.begin(), next_count, active.begin());
                active_count = next_count;
                if (active_count == 0) return kBig;
            }
            if (axis_char == 'H') x += delta; else y += delta;
        }
        if (x != total_dx || y != total_dy) return kBig;
        uint32_t best = kBig;
        for (const auto& target : targets)
            if (costs[target.first] < kBig)
                best = std::min(best, costs[target.first] + static_cast<uint32_t>(target.second));
        return best;
    }
};

TemplateCandidate parse_candidate(const std::string& encoded) {
    const size_t at = encoded.find('@');
    if (at == std::string::npos || encoded.size() < at + 3)
        throw std::runtime_error("bad candidate: " + encoded);
    TemplateCandidate result;
    result.pattern = encoded.substr(0, at);
    result.axes = result.pattern == "identity" ? std::vector<char>{} : [&]() {
        std::vector<char> axes;
        for (const std::string& item : split(result.pattern, '>')) axes.push_back(item.at(0));
        return axes;
    }();
    std::string values = encoded.substr(at + 1);
    result.mutation = values.back() == 'M';
    values.pop_back();
    const std::vector<std::string> fields = split(values, ':');
    if (fields.size() != result.axes.size() + 2) throw std::runtime_error("bad template width");
    result.h_trunk = std::stoi(fields[0]);
    result.v_trunk = std::stoi(fields[1]);
    for (size_t index = 2; index < fields.size(); ++index) result.local.push_back(std::stoi(fields[index]));
    return result;
}

template <class T>
T read_binary(std::istream& stream) {
    T value{};
    if (!stream.read(reinterpret_cast<char*>(&value), sizeof(value)))
        throw std::runtime_error("truncated runtime template table");
    return value;
}

class RuntimeGenerator {
public:
    RuntimeGenerator(const std::string& path, const std::string& selector_path) {
        load(path);
        if (!selector_path.empty()) load_selector(selector_path);
    }

    bool selected(const std::string& from_spec, const std::string& to_spec) const {
        if (selector_.empty()) return true;
        const Parsed from = parse_endpoint(from_spec);
        const Parsed to = parse_endpoint(to_spec);
        const int dx = to.x - from.x, dy = to.y - from.y;
        char direction[2] = {
            dx > 0 ? 'E' : dx < 0 ? 'W' : '0',
            dy > 0 ? 'N' : dy < 0 ? 'S' : '0'};
        const int cheb = std::max(std::abs(dx), std::abs(dy));
        const char* band = cheb <= 2 ? "2" : cheb <= 4 ? "4" :
                           cheb <= 8 ? "8" : cheb <= 16 ? "16" : "17+";
        uint64_t hash = 14695981039346656037ull;
        hash_stem(hash, from.port);
        hash_byte(hash, direction[0]);
        hash_byte(hash, direction[1]);
        hash_byte(hash, 0xffu);
        for (const unsigned char byte : std::string_view(band)) hash_byte(hash, byte);
        hash_byte(hash, 0xffu);
        return selector_.find(hash) != selector_.end();
    }

    const std::vector<TemplateCandidate>& generate(
        const std::string& from_spec, const std::string& to_spec, int limit) {
        const Parsed from = parse_endpoint(from_spec);
        const Parsed to = parse_endpoint(to_spec);
        const int dx = to.x - from.x, dy = to.y - from.y;
        const int direction = (dx > 0 ? 1 : dx < 0 ? 2 : 0) +
                              (dy > 0 ? 3 : dy < 0 ? 6 : 0);
        const int cheb = std::max(std::abs(dx), std::abs(dy));
        const std::string band = cheb <= 8 ? "0-8" : cheb <= 16 ? "9-16" : "17+";
        const uint64_t full_key = hash_full_key(
            from.port, to.port, direction, band);
        const auto cached = cache_.find(full_key);
        if (cached != cache_.end()) return cached->second;
        const std::string source_stem = stem(from.port);
        const std::string target_stem = stem(to.port);
        const std::string direction_text = std::to_string(direction);
        const std::array<std::vector<std::string>, 6> keys{{
            {source_stem, target_stem, direction_text, band},
            {source_stem, target_stem, direction_text},
            {source_stem, direction_text, band},
            {target_stem, direction_text, band},
            {direction_text, band},
            {direction_text},
        }};
        std::array<const Record*, 6> matched{};
        for (size_t level = 0; level < levels_.size(); ++level) {
            const auto found = levels_[level].find(hash_key(keys[level]));
            if (found != levels_[level].end()) matched[level] = &found->second;
        }

        if (++stamp_value_ == 0) {
            std::fill(stamps_.begin(), stamps_.end(), 0u);
            ++stamp_value_;
        }
        std::vector<uint16_t> ids;
        ids.reserve(224);
        const auto add = [&](uint16_t id) {
            if (stamps_[id] != stamp_value_) {
                stamps_[id] = stamp_value_;
                ids.push_back(id);
            }
        };
        for (uint16_t id : global_top_) add(id);
        for (const Record* record : matched)
            if (record) for (uint16_t id : record->top) add(id);

        struct Ranked { uint16_t id = 0; double score = 0.0; };
        std::vector<Ranked> ranked;
        ranked.reserve(ids.size());
        for (uint16_t id : ids) {
            float gain = 0.0f;
            for (const Record* record : matched) {
                if (!record) continue;
                const auto found = std::lower_bound(
                    record->gains.begin(), record->gains.end(), id,
                    [](const auto& item, uint16_t value) { return item.first < value; });
                if (found != record->gains.end() && found->first == id)
                    gain = std::max(gain, found->second);
            }
            ranked.push_back(Ranked{id, base_log_[id] + gain});
        }
        const auto score_order = [](const Ranked& left, const Ranked& right) {
            if (left.score != right.score) return left.score > right.score;
            return left.id < right.id;
        };
        std::sort(ranked.begin(), ranked.end(), score_order);
        if (ranked.size() > 64) ranked.resize(64);
        for (Ranked& item : ranked) item.score -= 0.35 * run_count_[item.id];
        std::sort(ranked.begin(), ranked.end(), score_order);
        if (static_cast<int>(ranked.size()) > limit) ranked.resize(limit);
        std::vector<TemplateCandidate> selected;
        selected.reserve(ranked.size());
        for (const Ranked& item : ranked) selected.push_back(templates_[item.id]);
        return cache_.emplace(full_key, std::move(selected)).first->second;
    }

private:
    struct Parsed { int x = 0, y = 0; std::string_view port; };
    struct Record {
        std::vector<uint16_t> top;
        std::vector<std::pair<uint16_t, float>> gains;
    };
    std::vector<TemplateCandidate> templates_;
    std::vector<double> base_log_;
    std::vector<int> run_count_;
    std::vector<uint16_t> global_top_;
    std::array<std::unordered_map<uint64_t, Record>, 6> levels_;
    std::unordered_map<uint64_t, std::vector<TemplateCandidate>> cache_;
    std::unordered_set<uint64_t> selector_;
    std::vector<uint32_t> stamps_;
    uint32_t stamp_value_ = 0;

    static Parsed parse_endpoint(const std::string& specification) {
        if (specification.size() < 8 || specification.rfind("SRB_", 0) != 0)
            throw std::runtime_error("bad runtime endpoint: " + specification);
        size_t cursor = 4;
        int x = 0, y = 0;
        if (cursor >= specification.size() || !std::isdigit(
                static_cast<unsigned char>(specification[cursor])))
            throw std::runtime_error("bad runtime endpoint: " + specification);
        while (cursor < specification.size() && std::isdigit(
                static_cast<unsigned char>(specification[cursor])))
            x = 10 * x + specification[cursor++] - '0';
        if (cursor >= specification.size() || specification[cursor++] != '_')
            throw std::runtime_error("bad runtime endpoint: " + specification);
        while (cursor < specification.size() && std::isdigit(
                static_cast<unsigned char>(specification[cursor])))
            y = 10 * y + specification[cursor++] - '0';
        if (cursor >= specification.size() || specification[cursor++] != '/')
            throw std::runtime_error("bad runtime endpoint: " + specification);
        return Parsed{x, y, std::string_view(specification).substr(cursor)};
    }

    static std::string stem(std::string_view port) {
        const size_t left = port.find('['), right = port.find(']');
        if (left == std::string::npos || right == std::string::npos || right < left)
            return std::string(port);
        return std::string(port.substr(0, left)) + "[*]" +
               std::string(port.substr(right + 1));
    }

    static void hash_byte(uint64_t& value, unsigned char byte) {
        value ^= byte;
        value *= 1099511628211ull;
    }

    static void hash_stem(uint64_t& value, std::string_view port) {
        const size_t left = port.find('['), right = port.find(']');
        if (left == std::string_view::npos || right == std::string_view::npos || right < left) {
            for (unsigned char byte : port) hash_byte(value, byte);
        } else {
            for (unsigned char byte : port.substr(0, left)) hash_byte(value, byte);
            hash_byte(value, '['); hash_byte(value, '*'); hash_byte(value, ']');
            for (unsigned char byte : port.substr(right + 1)) hash_byte(value, byte);
        }
        hash_byte(value, 0xffu);
    }

    static uint64_t hash_full_key(
        std::string_view source, std::string_view target, int direction,
        const std::string& band) {
        uint64_t result = 14695981039346656037ull;
        hash_stem(result, source);
        hash_stem(result, target);
        hash_byte(result, static_cast<unsigned char>('0' + direction));
        hash_byte(result, 0xffu);
        for (unsigned char byte : band) hash_byte(result, byte);
        hash_byte(result, 0xffu);
        return result;
    }

    static uint64_t hash_key(const std::vector<std::string>& values) {
        uint64_t result = 14695981039346656037ull;
        for (const std::string& value : values) {
            for (unsigned char byte : value) {
                hash_byte(result, byte);
            }
            hash_byte(result, 0xffu);
        }
        return result;
    }

    void load(const std::string& path) {
        std::ifstream stream(path, std::ios::binary);
        char magic[8];
        if (!stream.read(magic, sizeof(magic)) || std::memcmp(magic, "P6RTB001", 8) != 0)
            throw std::runtime_error("bad runtime template table: " + path);
        const uint32_t label_count = read_binary<uint32_t>(stream);
        std::vector<std::string> encoded(label_count);
        templates_.reserve(label_count);
        run_count_.reserve(label_count);
        for (uint32_t index = 0; index < label_count; ++index) {
            const uint16_t size = read_binary<uint16_t>(stream);
            encoded[index].resize(size);
            if (!stream.read(encoded[index].data(), size))
                throw std::runtime_error("truncated runtime template label");
            templates_.push_back(parse_candidate(encoded[index]));
            run_count_.push_back(templates_.back().axes.size());
        }
        const uint64_t global_total = read_binary<uint64_t>(stream);
        std::vector<uint32_t> global_counts(label_count);
        if (!stream.read(reinterpret_cast<char*>(global_counts.data()),
                         static_cast<std::streamsize>(label_count * sizeof(uint32_t))))
            throw std::runtime_error("truncated global template counts");
        const double denominator = global_total + 0.5 * label_count;
        base_log_.reserve(label_count);
        for (uint32_t count : global_counts)
            base_log_.push_back(std::log((count + 0.5) / denominator));
        const uint16_t global_top_count = read_binary<uint16_t>(stream);
        global_top_.resize(global_top_count);
        if (!stream.read(reinterpret_cast<char*>(global_top_.data()),
                         static_cast<std::streamsize>(global_top_count * sizeof(uint16_t))))
            throw std::runtime_error("truncated global top templates");
        const uint16_t level_count = read_binary<uint16_t>(stream);
        if (level_count != levels_.size()) throw std::runtime_error("runtime table level mismatch");
        for (auto& level : levels_) {
            const uint32_t record_count = read_binary<uint32_t>(stream);
            level.reserve(record_count);
            for (uint32_t record_index = 0; record_index < record_count; ++record_index) {
                const uint64_t key = read_binary<uint64_t>(stream);
                const uint32_t support = read_binary<uint32_t>(stream);
                const uint16_t top_count = read_binary<uint16_t>(stream);
                const uint16_t entry_count = read_binary<uint16_t>(stream);
                Record record;
                record.top.resize(top_count);
                if (!stream.read(reinterpret_cast<char*>(record.top.data()),
                                 static_cast<std::streamsize>(top_count * sizeof(uint16_t))))
                    throw std::runtime_error("truncated top template IDs");
                record.gains.reserve(entry_count);
                for (uint16_t entry = 0; entry < entry_count; ++entry) {
                    const uint16_t id = read_binary<uint16_t>(stream);
                    const uint32_t count = read_binary<uint32_t>(stream);
                    if (count < 2) continue;
                    const double base = std::exp(base_log_[id]);
                    const double posterior = (count + 32.0 * base) / (support + 32.0);
                    const double confidence = (count / (count + 2.0)) *
                                              (support / (support + 8.0));
                    const double gain = confidence * std::log(std::max(posterior / base, 1e-12));
                    if (gain > 0.0) record.gains.push_back({id, static_cast<float>(gain)});
                }
                level.emplace(key, std::move(record));
            }
        }
        stamps_.assign(label_count, 0u);
    }

    void load_selector(const std::string& path) {
        std::ifstream stream(path, std::ios::binary);
        char magic[8];
        if (!stream.read(magic, sizeof(magic)) || std::memcmp(magic, "P6SEL001", 8) != 0)
            throw std::runtime_error("bad runtime selector table: " + path);
        const uint32_t count = read_binary<uint32_t>(stream);
        selector_.reserve(count);
        for (uint32_t index = 0; index < count; ++index)
            selector_.insert(read_binary<uint64_t>(stream));
    }
};

std::vector<Request> load_requests(const std::string& path) {
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("cannot open candidates: " + path);
    std::string line;
    std::getline(stream, line);
    std::vector<Request> result;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t a = line.find(','), b = line.find(',', a + 1), c = line.find(',', b + 1);
        if (a == std::string::npos || b == std::string::npos)
            throw std::runtime_error("bad candidate CSV row");
        Request request;
        request.from = line.substr(0, a);
        request.to = line.substr(a + 1, b - a - 1);
        request.golden = std::stoi(line.substr(
            b + 1, c == std::string::npos ? c : c - b - 1));
        if (c != std::string::npos)
            for (const std::string& item : split(line.substr(c + 1), ';'))
                if (!item.empty()) request.candidates.push_back(parse_candidate(item));
        result.push_back(std::move(request));
    }
    return result;
}

struct PortalEvent {
    int x = 0;
    int phase = 0;
    int type_group = 0;
    std::string route;
};

struct PortalEvalRow {
    std::string from, to;
    int golden = 0, direction = 0, periods = 0;
    int true_source = -1, true_target = -1;
    int oracle_source_local = 0, oracle_core = 0, oracle_target_local = 0;
};

struct RankedPortal {
    uint32_t cost = kBig;
    uint32_t rank_cost = kBig;
    uint16_t event = 0;
};

std::vector<unsigned char> load_bytes(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open binary file: " + path);
    const std::streamsize size = input.tellg();
    if (size <= 0) throw std::runtime_error("empty binary file: " + path);
    input.seekg(0);
    std::vector<unsigned char> result(static_cast<size_t>(size));
    if (!input.read(reinterpret_cast<char*>(result.data()), size))
        throw std::runtime_error("cannot read binary file: " + path);
    return result;
}

std::vector<PortalEvalRow> load_portal_eval_rows(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open Portal decomposition: " + path);
    std::string line;
    if (!std::getline(input, line)) throw std::runtime_error("empty Portal decomposition");
    std::vector<PortalEvalRow> result;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::vector<std::string> fields = split(line, ',');
        if (fields.size() < 12) throw std::runtime_error("bad Portal decomposition row");
        result.push_back(PortalEvalRow{
            fields[0], fields[1], std::stoi(fields[2]), std::stoi(fields[3]),
            std::stoi(fields[4]), std::stoi(fields[7]), std::stoi(fields[8]),
            std::stoi(fields[9]), std::stoi(fields[10]), std::stoi(fields[11])});
    }
    return result;
}

Endpoint parse_coordinate(const std::string& specification) {
    const size_t slash = specification.find('/');
    const size_t underscore = specification.find('_', 4);
    if (specification.rfind("SRB_", 0) != 0 || slash == std::string::npos ||
        underscore == std::string::npos)
        throw std::runtime_error("bad endpoint coordinate: " + specification);
    return Endpoint{std::stoi(specification.substr(4, underscore - 4)),
                    std::stoi(specification.substr(underscore + 1, slash - underscore - 1)), -1};
}

std::array<std::vector<PortalEvent>, 2> load_portal_events(const std::string& path) {
    struct DirectionState { std::string route; int span = 0; };
    std::array<std::vector<DirectionState>, 2> states;
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open Portal state CSV: " + path);
    std::string line;
    if (!std::getline(input, line)) throw std::runtime_error("empty Portal state CSV");
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::vector<std::string> fields = split(line, ',');
        if (fields.size() < 5) throw std::runtime_error("bad Portal state row");
        const int dx = std::stoi(fields[3]), dy = std::stoi(fields[4]);
        if (dx == 0 && dy > 0) states[0].push_back({fields[1], dy});
        if (dx == 0 && dy < 0) states[1].push_back({fields[1], -dy});
    }
    for (const auto& direction_states : states)
        if (direction_states.size() != 40)
            throw std::runtime_error("Portal state CSV must contain 40 states per direction");
    std::array<std::vector<PortalEvent>, 2> result;
    const std::array<int, 12> xs{{70, 71, 72, 73, 74, 75, 90, 91, 92, 93, 94, 95}};
    for (int direction_index = 0; direction_index < 2; ++direction_index)
        for (int x : xs)
            for (size_t state_index = 0; state_index < states[direction_index].size(); ++state_index) {
                const DirectionState& state = states[direction_index][state_index];
                const int side = x < 76 ? 0 : 1;
                const int type_group = side * 40 + static_cast<int>(state_index);
                for (int phase = 1; phase <= state.span; ++phase)
                    result[direction_index].push_back(
                        PortalEvent{x, phase, type_group, state.route});
            }
    return result;
}

std::string portal_endpoint(const PortalEvent& event, int boundary, int direction) {
    return "SRB_" + std::to_string(event.x) + "_" +
           std::to_string(boundary + direction * event.phase) + "/" + event.route;
}

int rank_of_event(const std::vector<RankedPortal>& ranked, int event) {
    for (size_t index = 0; index < ranked.size(); ++index)
        if (ranked[index].event == event) return static_cast<int>(index + 1);
    return 0;
}

int run_portal_connector_eval(const Options& options, const Pricer& pricer,
                              RuntimeGenerator& generator) {
    using Clock = std::chrono::steady_clock;
    const std::vector<PortalEvalRow> rows = load_portal_eval_rows(options.portal_decomposition);
    const auto events = load_portal_events(options.portal_states);
    const std::vector<unsigned char> up_bytes = load_bytes(options.portal_up_closure);
    const std::vector<unsigned char> down_bytes = load_bytes(options.portal_down_closure);
    const p10::PackedPortalClosureView up(up_bytes.data(), up_bytes.size());
    const p10::PackedPortalClosureView down(down_bytes.data(), down_bytes.size());
    if (up.direction() != 1 || down.direction() != -1 || up.dimension() != 1920 ||
        down.dimension() != 1920 || events[0].size() != up.dimension() ||
        events[1].size() != down.dimension())
        throw std::runtime_error("Portal closure/event shape mismatch");

    const std::array<int, 12> all_beams{{
        1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 1920}};
    std::vector<int> beams;
    for (int beam : all_beams)
        if (beam <= options.portal_max_beam) beams.push_back(beam);
    const std::array<int, 7> joint_beams{{1, 2, 4, 8, 16, 32, 64}};
    struct Metrics {
        uint64_t reachable = 0, exact = 0, under = 0;
        double score = 0.0, mae = 0.0;
    };
    std::vector<Metrics> metrics(beams.size());
    std::array<Metrics, 7> joint_source_metrics{}, joint_target_metrics{};
    struct TypedStrategy { int sources = 0, per_type = 0; };
    const std::array<TypedStrategy, 6> typed_strategies{{
        {16, 1}, {32, 1}, {32, 2}, {32, 4}, {64, 1}, {64, 2}}};
    std::array<Metrics, 6> typed_metrics{};
    // A tiny closure-aware index supplements globally cheap target events
    // with exits that are structurally cheap for the current entry event.
    // The largest configuration needs only 16 uint16 target IDs per closure
    // row (well below 1 MiB for both directions and all five periods).
    struct ConditionalStrategy {
        int sources = 0, global_targets = 0, core_targets = 0;
    };
    const std::array<ConditionalStrategy, 6> conditional_strategies{{
        {16, 16, 4}, {16, 16, 8}, {32, 16, 4},
        {32, 16, 8}, {32, 16, 16}, {64, 16, 8}}};
    std::array<Metrics, 6> conditional_metrics{};
    std::array<uint64_t, 6> conditional_pairs{};
    Metrics branch_bound_metrics{};
    std::vector<uint32_t> branch_bound_rows, branch_bound_pairs;
    std::ofstream output(options.output);
    if (!output) throw std::runtime_error("cannot create Portal evaluation: " + options.output);
    output << "From,To,Golden,Direction,Periods,SourceReachable,TargetReachable,"
              "TrueSourceRank,TrueTargetRank,OracleSourceLocal,PredictedTrueSourceLocal,"
              "OracleTargetLocal,PredictedTrueTargetLocal";
    for (int beam : beams) output << ",Pred" << beam;
    for (int beam : joint_beams) output << ",JointSource" << beam;
    for (int beam : joint_beams) output << ",JointTarget" << beam;
    for (const TypedStrategy& strategy : typed_strategies)
        output << ",TypedS" << strategy.sources << "G" << strategy.per_type;
    for (const ConditionalStrategy& strategy : conditional_strategies)
        output << ",ConditionalS" << strategy.sources
               << "T" << strategy.global_targets
               << "C" << strategy.core_targets;
    output << ",BranchBound,BranchRows,BranchPairs,Transitions,ConnectorUs\n";

    uint64_t total_transitions = 0;
    double total_us = 0.0;
    std::array<std::array<std::vector<uint16_t>, 6>, 2> row_min_cache;
    std::array<std::array<std::vector<uint16_t>, 6>, 2> column_min_cache;
    std::array<std::array<std::vector<std::array<uint16_t, 16>>, 6>, 2>
        closure_top_target_cache;
    for (size_t row_index = 0; row_index < rows.size(); ++row_index) {
        const PortalEvalRow& row = rows[row_index];
        const int direction_index = row.direction > 0 ? 0 : 1;
        if (row.direction != 1 && row.direction != -1)
            throw std::runtime_error("bad Portal row direction");
        const auto& row_events = events[direction_index];
        const p10::PackedPortalClosureView& closure = row.direction > 0 ? up : down;
        if (row.periods <= 0 || row.periods > closure.periods())
            throw std::runtime_error("Portal row period outside closure");
        const Endpoint from = parse_coordinate(row.from), to = parse_coordinate(row.to);
        const int source_boundary = row.direction > 0
            ? ((from.y - 49 + 99) / 100) * 100 + 49
            : (from.y / 100) * 100;
        const int target_boundary = row.direction > 0
            ? ((to.y - 50) / 100) * 100 + 49
            : (to.y / 100 + 1) * 100;
        if (std::abs(target_boundary - source_boundary) / 100 != row.periods)
            throw std::runtime_error("Portal boundary inference disagrees with period count");

        std::vector<RankedPortal> source_ranked, target_ranked;
        source_ranked.reserve(row_events.size());
        target_ranked.reserve(row_events.size());
        uint32_t predicted_true_source = kBig, predicted_true_target = kBig;
        uint64_t transitions = 0;
        const auto started = Clock::now();
        for (size_t event_index = 0; event_index < row_events.size(); ++event_index) {
            const std::string entry = portal_endpoint(
                row_events[event_index], source_boundary, row.direction);
            const std::vector<TemplateCandidate>& candidates = generator.generate(
                row.from, entry, options.candidate_limit);
            int valid = 0;
            const uint32_t cost = pricer.query(row.from, entry, candidates, transitions, valid);
            if (static_cast<int>(event_index) == row.true_source) predicted_true_source = cost;
            if (cost < kBig)
                source_ranked.push_back(RankedPortal{
                    cost, cost, static_cast<uint16_t>(event_index)});

            const std::string exit = portal_endpoint(
                row_events[event_index], target_boundary, row.direction);
            const std::vector<TemplateCandidate>& target_candidates = generator.generate(
                exit, row.to, options.candidate_limit);
            valid = 0;
            const uint32_t target_cost = pricer.query(
                exit, row.to, target_candidates, transitions, valid);
            if (static_cast<int>(event_index) == row.true_target) predicted_true_target = target_cost;
            if (target_cost < kBig)
                target_ranked.push_back(RankedPortal{
                    target_cost, target_cost, static_cast<uint16_t>(event_index)});
        }
        // A local-only ranking often prefers a cheap event whose periodic
        // continuation is intrinsically expensive.  The exact row/column
        // minima are deterministic lower bounds that depend only on
        // direction, period, and event.  They can later be stored in less
        // than 75 KiB for both directions and all five periods.
        std::vector<uint16_t>& row_min = row_min_cache[direction_index][row.periods];
        std::vector<uint16_t>& column_min = column_min_cache[direction_index][row.periods];
        std::vector<std::array<uint16_t, 16>>& closure_top_targets =
            closure_top_target_cache[direction_index][row.periods];
        if (row_min.empty()) {
            row_min.assign(row_events.size(), kInf16);
            column_min.assign(row_events.size(), kInf16);
            closure_top_targets.resize(row_events.size());
            for (uint16_t source = 0; source < row_events.size(); ++source) {
                std::array<std::pair<uint16_t, uint16_t>, 16> best;
                best.fill({kInf16, kInf16});
                for (uint16_t target = 0; target < row_events.size(); ++target) {
                    const uint16_t core = closure.lookup(
                        static_cast<uint16_t>(row.periods), source, target);
                    row_min[source] = std::min(row_min[source], core);
                    column_min[target] = std::min(column_min[target], core);
                    const std::pair<uint16_t, uint16_t> candidate{core, target};
                    if (candidate < best.back()) {
                        size_t position = best.size() - 1;
                        while (position > 0 && candidate < best[position - 1]) {
                            best[position] = best[position - 1];
                            --position;
                        }
                        best[position] = candidate;
                    }
                }
                for (size_t index = 0; index < best.size(); ++index)
                    closure_top_targets[source][index] = best[index].second;
            }
        }
        for (RankedPortal& source : source_ranked)
            source.rank_cost += row_min[source.event];
        for (RankedPortal& target : target_ranked)
            target.rank_cost += column_min[target.event];
        const auto order = [](const RankedPortal& left, const RankedPortal& right) {
            return std::tuple<uint32_t, uint32_t, uint16_t>{
                       left.rank_cost, left.cost, left.event} <
                   std::tuple<uint32_t, uint32_t, uint16_t>{
                       right.rank_cost, right.cost, right.event};
        };
        std::sort(source_ranked.begin(), source_ranked.end(), order);
        std::sort(target_ranked.begin(), target_ranked.end(), order);
        std::vector<uint32_t> predictions(beams.size(), kBig);
        const auto update_metric = [&](Metrics& current, uint32_t prediction) {
            if (prediction >= kBig) return;
            ++current.reachable;
            const double error = std::abs(static_cast<double>(prediction) - row.golden);
            current.exact += prediction == static_cast<uint32_t>(row.golden);
            current.under += prediction < static_cast<uint32_t>(row.golden);
            current.mae += error;
            current.score += row.golden == 0
                ? static_cast<double>(prediction == 0)
                : 1.0 - std::tanh(4.0 * error / row.golden);
        };
        for (size_t beam_index = 0; beam_index < beams.size(); ++beam_index) {
            const size_t source_count = std::min<size_t>(beams[beam_index], source_ranked.size());
            const size_t target_count = std::min<size_t>(beams[beam_index], target_ranked.size());
            for (size_t source_index = 0; source_index < source_count; ++source_index)
                for (size_t target_index = 0; target_index < target_count; ++target_index) {
                    const RankedPortal& source = source_ranked[source_index];
                    const RankedPortal& target = target_ranked[target_index];
                    const uint32_t combined = source.cost +
                        closure.lookup(static_cast<uint16_t>(row.periods), source.event, target.event) +
                        target.cost;
                    predictions[beam_index] = std::min(predictions[beam_index], combined);
                }
            update_metric(metrics[beam_index], predictions[beam_index]);
        }
        // Joint expansion keeps only K events on one side but scans every
        // reachable local event on the other side.  This identifies whether
        // the remaining loss is entry selection or exit selection, and is a
        // precursor to a factored row/column index rather than a 512x512 Beam.
        std::array<uint32_t, 7> joint_source_predictions{}, joint_target_predictions{};
        joint_source_predictions.fill(kBig);
        joint_target_predictions.fill(kBig);
        uint32_t joint_best = kBig;
        size_t joint_cursor = 0;
        const size_t source_limit = std::min<size_t>(joint_beams.back(), source_ranked.size());
        for (size_t source_index = 0; source_index < source_limit; ++source_index) {
            const RankedPortal& source = source_ranked[source_index];
            for (const RankedPortal& target : target_ranked)
                joint_best = std::min(joint_best, source.cost +
                    closure.lookup(static_cast<uint16_t>(row.periods), source.event, target.event) +
                    target.cost);
            while (joint_cursor < joint_beams.size() &&
                   static_cast<size_t>(joint_beams[joint_cursor]) == source_index + 1)
                joint_source_predictions[joint_cursor++] = joint_best;
        }
        joint_best = kBig;
        joint_cursor = 0;
        const size_t target_limit = std::min<size_t>(joint_beams.back(), target_ranked.size());
        for (size_t target_index = 0; target_index < target_limit; ++target_index) {
            const RankedPortal& target = target_ranked[target_index];
            for (const RankedPortal& source : source_ranked)
                joint_best = std::min(joint_best, source.cost +
                    closure.lookup(static_cast<uint16_t>(row.periods), source.event, target.event) +
                    target.cost);
            while (joint_cursor < joint_beams.size() &&
                   static_cast<size_t>(joint_beams[joint_cursor]) == target_index + 1)
                joint_target_predictions[joint_cursor++] = joint_best;
        }
        for (size_t index = 0; index < joint_beams.size(); ++index) {
            update_metric(joint_source_metrics[index], joint_source_predictions[index]);
            update_metric(joint_target_metrics[index], joint_target_predictions[index]);
        }
        // Inspired by lightweight FPGA routing: compare resources only
        // within the same type, retain a small number of winners per type,
        // and preserve type diversity.  The 1920 target events form 80
        // deterministic groups: left/right corridor x 40 direction states.
        std::array<std::vector<RankedPortal>, 80> target_types;
        for (const RankedPortal& target : target_ranked)
            target_types[row_events[target.event].type_group].push_back(target);
        std::array<uint32_t, 6> typed_predictions{};
        typed_predictions.fill(kBig);
        for (size_t strategy_index = 0; strategy_index < typed_strategies.size(); ++strategy_index) {
            const TypedStrategy strategy = typed_strategies[strategy_index];
            const size_t source_count = std::min<size_t>(strategy.sources, source_ranked.size());
            uint32_t best = kBig;
            for (size_t source_index = 0; source_index < source_count; ++source_index) {
                const RankedPortal& source = source_ranked[source_index];
                for (const auto& group : target_types) {
                    const size_t target_count = std::min<size_t>(strategy.per_type, group.size());
                    for (size_t target_index = 0; target_index < target_count; ++target_index) {
                        const RankedPortal& target = group[target_index];
                        best = std::min(best, source.cost +
                            closure.lookup(static_cast<uint16_t>(row.periods),
                                           source.event, target.event) + target.cost);
                    }
                }
            }
            typed_predictions[strategy_index] = best;
            update_metric(typed_metrics[strategy_index], best);
        }
        // Conditional target shortlists combine query-local winners with a
        // precomputed per-entry closure shortlist.  This tests whether a few
        // hundred exact combinations can replace an indiscriminate Beam^2.
        std::vector<uint32_t> target_cost_by_event(row_events.size(), kBig);
        for (const RankedPortal& target : target_ranked)
            target_cost_by_event[target.event] = target.cost;
        std::array<uint32_t, 6> conditional_predictions{};
        conditional_predictions.fill(kBig);
        for (size_t strategy_index = 0;
             strategy_index < conditional_strategies.size(); ++strategy_index) {
            const ConditionalStrategy strategy = conditional_strategies[strategy_index];
            const size_t source_count = std::min<size_t>(strategy.sources, source_ranked.size());
            const size_t global_count = std::min<size_t>(
                strategy.global_targets, target_ranked.size());
            uint32_t best = kBig;
            uint64_t pairs = 0;
            for (size_t source_index = 0; source_index < source_count; ++source_index) {
                const RankedPortal& source = source_ranked[source_index];
                for (size_t target_index = 0; target_index < global_count; ++target_index) {
                    const RankedPortal& target = target_ranked[target_index];
                    best = std::min(best, source.cost +
                        closure.lookup(static_cast<uint16_t>(row.periods),
                                       source.event, target.event) + target.cost);
                    ++pairs;
                }
                for (int core_index = 0; core_index < strategy.core_targets; ++core_index) {
                    const uint16_t target_event =
                        closure_top_targets[source.event][core_index];
                    if (target_event == kInf16 || target_cost_by_event[target_event] >= kBig)
                        continue;
                    best = std::min(best, source.cost +
                        closure.lookup(static_cast<uint16_t>(row.periods),
                                       source.event, target_event) +
                        target_cost_by_event[target_event]);
                    ++pairs;
                }
            }
            conditional_predictions[strategy_index] = best;
            conditional_pairs[strategy_index] += pairs;
            update_metric(conditional_metrics[strategy_index], best);
        }

        // Certified layered expansion.  For any fixed source event,
        //   source_local + row_min[source] + min(target_local)
        // is an admissible lower bound.  Once the next row cannot beat the
        // incumbent, all remaining rows are safely pruned.  A 16x16 seed
        // supplies an inexpensive incumbent; the reported pair count includes
        // seed lookups and excludes duplicate seed cells during row scans.
        const size_t seed_sources = std::min<size_t>(16, source_ranked.size());
        const size_t seed_targets = std::min<size_t>(16, target_ranked.size());
        uint32_t branch_prediction = kBig;
        uint32_t branch_pairs = 0, expanded_rows = 0;
        for (size_t source_index = 0; source_index < seed_sources; ++source_index)
            for (size_t target_index = 0; target_index < seed_targets; ++target_index) {
                const RankedPortal& source = source_ranked[source_index];
                const RankedPortal& target = target_ranked[target_index];
                branch_prediction = std::min(branch_prediction, source.cost +
                    closure.lookup(static_cast<uint16_t>(row.periods),
                                   source.event, target.event) + target.cost);
                ++branch_pairs;
            }
        const uint32_t minimum_target_local = target_ranked.empty()
            ? kBig : std::min_element(
                target_ranked.begin(), target_ranked.end(),
                [](const RankedPortal& left, const RankedPortal& right) {
                    return left.cost < right.cost;
                })->cost;
        for (size_t source_index = 0; source_index < source_ranked.size(); ++source_index) {
            const RankedPortal& source = source_ranked[source_index];
            const uint64_t lower_bound = static_cast<uint64_t>(source.cost) +
                row_min[source.event] + minimum_target_local;
            if (lower_bound >= branch_prediction) break;
            ++expanded_rows;
            const size_t begin_target = source_index < seed_sources ? seed_targets : 0;
            for (size_t target_index = begin_target;
                 target_index < target_ranked.size(); ++target_index) {
                const RankedPortal& target = target_ranked[target_index];
                branch_prediction = std::min(branch_prediction, source.cost +
                    closure.lookup(static_cast<uint16_t>(row.periods),
                                   source.event, target.event) + target.cost);
                ++branch_pairs;
            }
        }
        update_metric(branch_bound_metrics, branch_prediction);
        branch_bound_rows.push_back(expanded_rows);
        branch_bound_pairs.push_back(branch_pairs);
        const double elapsed_us = std::chrono::duration<double, std::micro>(
            Clock::now() - started).count();
        total_us += elapsed_us;
        total_transitions += transitions;
        output << row.from << ',' << row.to << ',' << row.golden << ',' << row.direction << ','
               << row.periods << ',' << source_ranked.size() << ',' << target_ranked.size() << ','
               << rank_of_event(source_ranked, row.true_source) << ','
               << rank_of_event(target_ranked, row.true_target) << ','
               << row.oracle_source_local << ',';
        if (predicted_true_source >= kBig) output << -1; else output << predicted_true_source;
        output << ',' << row.oracle_target_local << ',';
        if (predicted_true_target >= kBig) output << -1; else output << predicted_true_target;
        for (uint32_t prediction : predictions) {
            output << ',';
            if (prediction >= kBig) output << -1; else output << prediction;
        }
        for (uint32_t prediction : joint_source_predictions) {
            output << ',';
            if (prediction >= kBig) output << -1; else output << prediction;
        }
        for (uint32_t prediction : joint_target_predictions) {
            output << ',';
            if (prediction >= kBig) output << -1; else output << prediction;
        }
        for (uint32_t prediction : typed_predictions) {
            output << ',';
            if (prediction >= kBig) output << -1; else output << prediction;
        }
        for (uint32_t prediction : conditional_predictions) {
            output << ',';
            if (prediction >= kBig) output << -1; else output << prediction;
        }
        output << ',';
        if (branch_prediction >= kBig) output << -1; else output << branch_prediction;
        output << ',' << expanded_rows << ',' << branch_pairs
               << ',' << transitions << ',' << std::fixed << std::setprecision(3)
               << elapsed_us << '\n';
        if ((row_index + 1) % 10 == 0 || row_index + 1 == rows.size())
            std::cerr << "portal_rows=" << (row_index + 1) << '/' << rows.size()
                      << " mean_us=" << (total_us / (row_index + 1)) << '\n';
    }
    const double denominator = std::max<size_t>(rows.size(), 1);
    std::cerr << std::fixed << std::setprecision(6);
    for (size_t index = 0; index < beams.size(); ++index) {
        const Metrics& current = metrics[index];
        std::cerr << "beam=" << beams[index]
                  << " reachable=" << current.reachable
                  << " exact=" << current.exact
                  << " under=" << current.under
                  << " accuracy=" << (100.0 * current.score / denominator)
                  << " mae=" << (current.mae / denominator) << '\n';
    }
    for (size_t index = 0; index < joint_beams.size(); ++index) {
        const Metrics& source = joint_source_metrics[index];
        const Metrics& target = joint_target_metrics[index];
        std::cerr << "joint_source=" << joint_beams[index]
                  << " exact=" << source.exact << " under=" << source.under
                  << " accuracy=" << (100.0 * source.score / denominator)
                  << " mae=" << (source.mae / denominator)
                  << " joint_target=" << joint_beams[index]
                  << " exact=" << target.exact << " under=" << target.under
                  << " accuracy=" << (100.0 * target.score / denominator)
                  << " mae=" << (target.mae / denominator) << '\n';
    }
    for (size_t index = 0; index < typed_strategies.size(); ++index) {
        const TypedStrategy strategy = typed_strategies[index];
        const Metrics& current = typed_metrics[index];
        std::cerr << "typed_sources=" << strategy.sources
                  << " per_type=" << strategy.per_type
                  << " exact=" << current.exact << " under=" << current.under
                  << " accuracy=" << (100.0 * current.score / denominator)
                  << " mae=" << (current.mae / denominator) << '\n';
    }
    for (size_t index = 0; index < conditional_strategies.size(); ++index) {
        const ConditionalStrategy strategy = conditional_strategies[index];
        const Metrics& current = conditional_metrics[index];
        std::cerr << "conditional_sources=" << strategy.sources
                  << " global_targets=" << strategy.global_targets
                  << " core_targets=" << strategy.core_targets
                  << " exact=" << current.exact << " under=" << current.under
                  << " accuracy=" << (100.0 * current.score / denominator)
                  << " mae=" << (current.mae / denominator)
                  << " mean_pairs=" << (conditional_pairs[index] / denominator) << '\n';
    }
    std::vector<uint32_t> sorted_branch_rows = branch_bound_rows;
    std::vector<uint32_t> sorted_branch_pairs = branch_bound_pairs;
    std::sort(sorted_branch_rows.begin(), sorted_branch_rows.end());
    std::sort(sorted_branch_pairs.begin(), sorted_branch_pairs.end());
    const auto percentile = [](const std::vector<uint32_t>& values, double fraction) {
        if (values.empty()) return 0u;
        const size_t index = std::min<size_t>(
            values.size() - 1, static_cast<size_t>(std::ceil(fraction * values.size())) - 1);
        return values[index];
    };
    uint64_t branch_row_sum = 0, branch_pair_sum = 0;
    for (uint32_t value : branch_bound_rows) branch_row_sum += value;
    for (uint32_t value : branch_bound_pairs) branch_pair_sum += value;
    std::cerr << "branch_bound_exact=" << branch_bound_metrics.exact
              << " under=" << branch_bound_metrics.under
              << " accuracy=" << (100.0 * branch_bound_metrics.score / denominator)
              << " mae=" << (branch_bound_metrics.mae / denominator)
              << " mean_rows=" << (branch_row_sum / denominator)
              << " p50_rows=" << percentile(sorted_branch_rows, 0.50)
              << " p95_rows=" << percentile(sorted_branch_rows, 0.95)
              << " max_rows=" << (sorted_branch_rows.empty() ? 0 : sorted_branch_rows.back())
              << " mean_pairs=" << (branch_pair_sum / denominator)
              << " p95_pairs=" << percentile(sorted_branch_pairs, 0.95) << '\n';
    std::cerr << "mean_connector_us=" << (total_us / denominator)
              << " mean_transitions=" << (static_cast<double>(total_transitions) / denominator)
              << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        const Pricer pricer(options);
        std::unique_ptr<RuntimeGenerator> generator;
        if (!options.runtime_table.empty())
            generator = std::make_unique<RuntimeGenerator>(
                options.runtime_table, options.selector_table);
        if (!options.portal_decomposition.empty()) {
            if (!generator) throw std::runtime_error("Portal evaluation requires runtime generator");
            return run_portal_connector_eval(options, pricer, *generator);
        }
        const std::vector<Request> requests = load_requests(options.candidates);
        if (generator && options.warm_cache)
            for (const Request& request : requests)
                if (generator->selected(request.from, request.to))
                    generator->generate(request.from, request.to, options.candidate_limit);
        std::ofstream output(options.output);
        if (!output) throw std::runtime_error("cannot open output: " + options.output);
        output << "From,To,Golden,Predicted,Candidates,ValidCandidates,Transitions,GenerateUs,ElapsedUs\n";
        uint64_t total_transitions = 0, total_candidates = 0, processed = 0, selected = 0;
        double total_us = 0.0, total_generate_us = 0.0, score = 0.0, mae = 0.0;
        uint64_t exact = 0, reachable = 0;
        for (int pass = 0; pass < options.passes; ++pass)
        for (const Request& request : requests) {
            uint64_t transitions = 0;
            int valid = 0;
            const auto started = std::chrono::steady_clock::now();
            const std::vector<TemplateCandidate>* candidates = &request.candidates;
            double generate_us = 0.0;
            const bool selected_for_pricing = !generator ||
                generator->selected(request.from, request.to);
            if (selected_for_pricing) ++selected;
            if (generator && selected_for_pricing) {
                const auto generate_started = std::chrono::steady_clock::now();
                candidates = &generator->generate(
                    request.from, request.to, options.candidate_limit);
                generate_us = std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - generate_started).count();
            }
            const uint32_t prediction = selected_for_pricing
                ? pricer.query(request.from, request.to, *candidates, transitions, valid)
                : kBig;
            const double elapsed_us = std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - started).count();
            output << request.from << ',' << request.to << ',' << request.golden << ',';
            if (prediction >= kBig) output << -1;
            else output << prediction;
            const size_t used_candidates = std::min<size_t>(
                options.candidate_limit, candidates->size());
            output << ',' << used_candidates << ',' << valid << ','
                   << transitions << ',' << std::fixed << std::setprecision(3)
                   << generate_us << ',' << elapsed_us << '\n';
            total_candidates += used_candidates;
            total_transitions += transitions;
            total_us += elapsed_us;
            total_generate_us += generate_us;
            ++processed;
            if (prediction < kBig) {
                ++reachable;
                const double error = std::abs(static_cast<double>(prediction) - request.golden);
                exact += prediction == static_cast<uint32_t>(request.golden);
                mae += error;
                score += request.golden == 0
                    ? static_cast<double>(prediction == 0)
                    : 1.0 - std::tanh(4.0 * error / request.golden);
            }
        }
        const double denominator = std::max<uint64_t>(reachable, 1);
        std::cerr << "rows=" << processed << " selected=" << selected
                  << " reachable=" << reachable
                  << " exact=" << exact << " accuracy=" << (100.0 * score / denominator)
                  << " mae=" << (mae / denominator)
                  << " mean_candidates=" << (static_cast<double>(total_candidates) / processed)
                  << " mean_transitions=" << (static_cast<double>(total_transitions) / processed)
                  << " mean_generate_us=" << (total_generate_us / processed)
                  << " mean_us=" << (total_us / processed) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
