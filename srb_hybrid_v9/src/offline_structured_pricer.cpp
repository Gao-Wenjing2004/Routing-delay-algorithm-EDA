#include "srb_core_v9.hpp"

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
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr int kStates = 160;
constexpr int kRadius = 32;
constexpr int kWidth = 65;
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
};

struct OutputNet { int route = -1, dx = 0, dy = 0; };
struct Source { int state = -1, dx = 0, dy = 0, cost = 0; };
struct Gap { bool vertical = false; int site = 0, delay = 0; };
struct Rect { int left = 0, right = 0, lower = 0, upper = 0; };

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
        else throw std::runtime_error("unknown argument: " + argument);
    }
    if (result.model.empty() || result.axis.empty() || result.candidates.empty() || result.output.empty())
        throw std::runtime_error(
            "usage: offline_structured_pricer --model prpr_model.json --axis axis.bin "
            "--candidates candidates.csv --output audit.csv [--candidate-limit 32] "
            "[--state-beam 32] [--edge-beam 160] [--runtime-table table.bin] "
            "[--selector-table selector.bin] [--passes 1] [--warm-cache]");
    if (result.state_beam <= 0 || result.state_beam > kStates)
        throw std::runtime_error("state beam must be in [1,160]");
    if (result.edge_beam <= 0 || result.edge_beam > kStates)
        throw std::runtime_error("edge beam must be in [1,160]");
    if (result.candidate_limit <= 0)
        throw std::runtime_error("candidate limit must be positive");
    if (result.passes <= 0)
        throw std::runtime_error("passes must be positive");
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
          candidate_limit_(options.candidate_limit) {
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
        const std::vector<Source> sources = source_candidates(from.port);
        const auto& targets = target_arcs_[to.port];
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
                    from.x, from.y, total_dx, total_dy, source, targets,
                    candidate.axes, deltas, transition_count);
                if (priced < kBig) {
                    ++valid_candidates;
                    best = std::min(best, priced);
                }
            }
        }
        return best;
    }

private:
    struct EdgeChoice { uint16_t target = 0, cost = kInf16; };
    int width_ = 0, height_ = 0, state_beam_ = 32, edge_beam_ = kStates;
    int candidate_limit_ = 32;
    std::unordered_map<std::string, int> port_ids_;
    std::vector<int> port_to_input_, input_to_state_;
    std::vector<OutputNet> output_nets_;
    std::vector<std::vector<std::pair<int, int>>> direct_arcs_, target_arcs_;
    std::vector<Gap> gaps_;
    std::vector<Rect> blocks_;
    std::vector<uint16_t> axis_;
    std::vector<EdgeChoice> top_edges_;

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
                iget(item, "left"), iget(item, "right"), iget(item, "lower"), iget(item, "upper")});
    }

    void load_axis(const std::string& path) {
        const size_t count = 2ull * kStates * kWidth * kStates;
        axis_.resize(count);
        std::ifstream stream(path, std::ios::binary);
        if (!stream || !stream.read(reinterpret_cast<char*>(axis_.data()),
                                    static_cast<std::streamsize>(count * sizeof(uint16_t))))
            throw std::runtime_error("cannot read axis table: " + path);
    }

    const uint16_t* table(int axis, int state, int delta) const {
        const size_t one_axis = static_cast<size_t>(kStates) * kWidth * kStates;
        return axis_.data() + axis * one_axis +
            (static_cast<size_t>(state) * kWidth + delta + kRadius) * kStates;
    }

    void build_top_edges() {
        if (edge_beam_ == kStates) return;
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

    std::vector<Source> source_candidates(int port) const {
        std::vector<Source> result;
        const int iid = port_to_input_[port];
        if (iid >= 0) {
            const int route = input_to_state_[iid];
            if (route >= 0) return {Source{route, 0, 0, 0}};
            for (const auto& arc : direct_arcs_[iid]) {
                const OutputNet& net = output_nets_[arc.first];
                if (net.route >= 0) result.push_back(Source{net.route, net.dx, net.dy, arc.second});
            }
            return result;
        }
        const OutputNet& net = output_nets_[port];
        if (net.route >= 0) result.push_back(Source{net.route, net.dx, net.dy, 0});
        return result;
    }

    bool segment_extra(int x, int y, char axis, int delta, uint32_t& extra) const {
        const int nx = x + (axis == 'H' ? delta : 0);
        const int ny = y + (axis == 'V' ? delta : 0);
        if (nx < 0 || nx >= width_ || ny < 0 || ny >= height_) return false;
        const int min_x = std::min(x, nx), max_x = std::max(x, nx);
        const int min_y = std::min(y, ny), max_y = std::max(y, ny);
        for (const Rect& block : blocks_)
            if (min_x <= block.right && max_x >= block.left &&
                min_y <= block.upper && max_y >= block.lower) return false;
        extra = 0;
        for (const Gap& gap : gaps_) {
            if (axis == 'H' && gap.vertical && min_x <= gap.site && gap.site < max_x)
                extra += gap.delay;
            else if (axis == 'V' && !gap.vertical && min_y <= gap.site && gap.site < max_y)
                extra += gap.delay;
        }
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
        return std::all_of(values.begin(), values.end(), [](int value) {
            return -kRadius <= value && value <= kRadius;
        });
    }

    uint32_t price_candidate(
        int sx, int sy, int total_dx, int total_dy, const Source& source,
        const std::vector<std::pair<int, int>>& targets, const std::vector<char>& axes,
        const std::vector<int>& deltas, uint64_t& transition_count) const {
        const char entry_axis = source.dx ? 'H' : source.dy ? 'V' : 0;
        if (entry_axis && (axes.empty() || axes.front() != entry_axis)) return kBig;
        uint32_t entry_extra = 0;
        if (entry_axis && !segment_extra(sx, sy, entry_axis, source.dx ? source.dx : source.dy, entry_extra))
            return kBig;

        std::array<uint32_t, kStates> costs, next;
        std::array<int, kStates> active{}, next_active{};
        costs.fill(kBig);
        costs[source.state] = static_cast<uint32_t>(source.cost) + entry_extra;
        active[0] = source.state;
        int active_count = 1;
        int x = source.dx, y = source.dy;
        for (size_t run = 0; run < axes.size(); ++run) {
            const char axis_char = axes[run];
            const int delta = deltas[run];
            const bool source_noop = run == 0 && entry_axis == axis_char && delta == 0;
            if (!source_noop) {
                uint32_t extra = 0;
                if (!segment_extra(sx + x, sy + y, axis_char, delta, extra)) return kBig;
                next.fill(kBig);
                const bool final_run = run + 1 == axes.size();
                if (final_run) {
                    uint32_t best = kBig;
                    for (int active_index = 0; active_index < active_count; ++active_index) {
                        const int source_state = active[active_index];
                        const uint16_t* edges = table(
                            axis_char == 'H' ? 0 : 1, source_state, delta);
                        transition_count += targets.size();
                        for (const auto& target : targets) {
                            const uint16_t edge = edges[target.first];
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
                    const uint16_t* edges = table(axis_char == 'H' ? 0 : 1, source_state, delta);
                    if (edge_beam_ == kStates) {
                        transition_count += kStates;
                        for (int target_state = 0; target_state < kStates; ++target_state)
                            if (edges[target_state] != kInf16)
                                next[target_state] = std::min(
                                    next[target_state], costs[source_state] + edges[target_state] + extra);
                    } else {
                        transition_count += edge_beam_;
                        const EdgeChoice* choices = top_edges(
                            axis_char == 'H' ? 0 : 1, source_state, delta);
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

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        const Pricer pricer(options);
        std::unique_ptr<RuntimeGenerator> generator;
        if (!options.runtime_table.empty())
            generator = std::make_unique<RuntimeGenerator>(
                options.runtime_table, options.selector_table);
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
