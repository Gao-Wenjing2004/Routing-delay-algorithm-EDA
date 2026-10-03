#pragma once
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <limits>
#include <memory>
#include <queue>
#include <stdexcept>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <type_traits>
#include <vector>

static constexpr uint32_t INF32 = std::numeric_limits<uint32_t>::max() / 4;
static constexpr uint32_t PARENT_ROOT_INPUT  = std::numeric_limits<uint32_t>::max();
static constexpr uint32_t PARENT_ROOT_OUTPUT = std::numeric_limits<uint32_t>::max() - 1;
static constexpr uint32_t PARENT_ROOT_NONROUTING_INPUT = std::numeric_limits<uint32_t>::max() - 2;
static constexpr uint16_t NO_U16 = std::numeric_limits<uint16_t>::max();

// -------------------------- minimal JSON parser --------------------------
// Standard-library only. Enough for the five SRB JSON files.
struct Json {
    enum class Type { Null, Bool, Int, String, Array, Object } type = Type::Null;
    bool b = false;
    int64_t i = 0;
    std::string s;
    std::vector<Json> a;
    std::unordered_map<std::string, std::unique_ptr<Json>> o;

    const Json& at(const std::string& key) const {
        auto it = o.find(key);
        if (it == o.end()) throw std::runtime_error("JSON key not found: " + key);
        return *it->second;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : text_(text) {}

    Json parse() {
        skip_ws();
        Json v = parse_value();
        skip_ws();
        if (pos_ != text_.size()) error("trailing characters");
        return v;
    }

private:
    const std::string& text_;
    size_t pos_ = 0;

    [[noreturn]] void error(const std::string& msg) const {
        throw std::runtime_error("JSON parse error at byte " + std::to_string(pos_) + ": " + msg);
    }

    void skip_ws() {
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') ++pos_;
            else break;
        }
    }

    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

    bool consume(char c) {
        if (peek() == c) { ++pos_; return true; }
        return false;
    }

    bool match_literal(const char* lit) {
        size_t n = std::char_traits<char>::length(lit);
        if (text_.compare(pos_, n, lit) == 0) {
            pos_ += n;
            return true;
        }
        return false;
    }

    Json parse_value() {
        skip_ws();
        char c = peek();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') {
            Json v; v.type = Json::Type::String; v.s = parse_string(); return v;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            Json v; v.type = Json::Type::Int; v.i = parse_int(); return v;
        }
        if (match_literal("true"))  { Json v; v.type = Json::Type::Bool; v.b = true;  return v; }
        if (match_literal("false")) { Json v; v.type = Json::Type::Bool; v.b = false; return v; }
        if (match_literal("null"))  return Json{};
        error("unexpected token");
    }

    Json parse_object() {
        Json v; v.type = Json::Type::Object;
        if (!consume('{')) error("expected '{'");
        skip_ws();
        if (consume('}')) return v;
        while (true) {
            skip_ws();
            if (peek() != '"') error("expected string key");
            std::string key = parse_string();
            skip_ws();
            if (!consume(':')) error("expected ':'");
            Json value = parse_value();
            v.o.emplace(std::move(key), std::make_unique<Json>(std::move(value)));
            skip_ws();
            if (consume('}')) return v;
            if (!consume(',')) error("expected ',' or '}'");
        }
    }

    Json parse_array() {
        Json v; v.type = Json::Type::Array;
        if (!consume('[')) error("expected '['");
        skip_ws();
        if (consume(']')) return v;
        while (true) {
            v.a.push_back(parse_value());
            skip_ws();
            if (consume(']')) return v;
            if (!consume(',')) error("expected ',' or ']'");
        }
    }

    std::string parse_string() {
        if (!consume('"')) error("expected quote");
        std::string out;
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"') return out;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) error("bad escape");
            char e = text_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) error("short unicode escape");
                    unsigned code = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = text_[pos_++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code += h - '0';
                        else if (h >= 'a' && h <= 'f') code += h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') code += h - 'A' + 10;
                        else error("bad unicode escape");
                    }
                    if (code <= 0x7f) out.push_back(static_cast<char>(code));
                    else if (code <= 0x7ff) {
                        out.push_back(static_cast<char>(0xc0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                    } else {
                        out.push_back(static_cast<char>(0xe0 | (code >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                    }
                    break;
                }
                default: error("unsupported escape");
            }
        }
        error("unterminated string");
    }

    int64_t parse_int() {
        size_t begin = pos_;
        if (peek() == '-') ++pos_;
        if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') error("bad integer");
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        return std::stoll(text_.substr(begin, pos_ - begin));
    }
};

static Json load_json(const std::string& path) {
    std::ifstream fin(path, std::ios::binary);
    if (!fin) throw std::runtime_error("cannot open: " + path);
    fin.seekg(0, std::ios::end);
    std::streamsize n = fin.tellg();
    fin.seekg(0, std::ios::beg);
    std::string text(static_cast<size_t>(n), '\0');
    if (n > 0) fin.read(&text[0], n);
    return JsonParser(text).parse();
}

static std::string sget(const Json& o, const std::string& key) {
    const Json& v = o.at(key);
    if (v.type != Json::Type::String) throw std::runtime_error("JSON key is not string: " + key);
    return v.s;
}

static int iget(const Json& o, const std::string& key) {
    const Json& v = o.at(key);
    if (v.type != Json::Type::Int) throw std::runtime_error("JSON key is not integer: " + key);
    return static_cast<int>(v.i);
}

static bool bget(const Json& o, const std::string& key) {
    const Json& v = o.at(key);
    if (v.type != Json::Type::Bool) throw std::runtime_error("JSON key is not bool: " + key);
    return v.b;
}

// ------------------------------- SRB model -------------------------------
struct Coord {
    int16_t x = 0;
    int16_t y = 0;
};

struct GapLine {
    bool vertical = false; // true: site=x, gap is to the right of x
    int16_t site = 0;      // false: site=y, gap is above y
    uint16_t delay = 0;
};

struct Block {
    int16_t lower = 0;
    int16_t upper = 0;
    int16_t left = 0;
    int16_t right = 0;
    bool vertical_crossable = false;
    uint16_t vertical_cross_delay = 0;
    bool horizontal_crossable = false;
    uint16_t horizontal_cross_delay = 0;
};

struct NetRule {
    uint16_t from_output = 0; // global port id
    uint16_t dst_input = 0;   // compressed input id
    int16_t dx = 0;
    int16_t dy = 0;
};

struct MacroEdge {
    uint16_t out_port = 0;    // output used by Input -> Output Arc
    uint16_t next_input = 0;  // Input reached after Net
    uint16_t arc_delay = 0;
    uint16_t net_id = 0;
};

struct ReverseMacroEdge {
    uint16_t source_route = 0;
    uint16_t net_id = 0;
    uint16_t arc_delay = 0;
};

struct GeomMove {
    int16_t dx = 0;
    int16_t dy = 0;
    uint16_t rep_net = 0;
    uint16_t min_arc = NO_U16;
};

struct QueryResult {
    bool reachable = false;
    bool budget_exhausted = false;
    uint32_t delay = INF32;
    uint64_t expanded = 0;
    std::vector<std::string> path;
};

struct HeapItem {
    uint32_t f = 0;      // A* priority = g + h
    uint32_t g = 0;      // exact distance from source
    uint32_t state = 0;
};

class MinHeap {
public:
    void clear() { a_.clear(); }
    void reserve(size_t n) { a_.reserve(n); }
    bool empty() const { return a_.empty(); }
    const HeapItem& top() const { return a_.front(); }

    void push(HeapItem v) {
        a_.push_back(v);
        size_t i = a_.size() - 1;
        while (i > 0) {
            size_t p = (i - 1) >> 1;
            if (!less(v, a_[p])) break;
            a_[i] = a_[p];
            i = p;
        }
        a_[i] = v;
    }

    HeapItem pop() {
        HeapItem ret = a_.front();
        HeapItem v = a_.back();
        a_.pop_back();
        if (a_.empty()) return ret;

        size_t i = 0;
        while (true) {
            size_t l = i * 2 + 1;
            if (l >= a_.size()) break;
            size_t r = l + 1;
            size_t c = (r < a_.size() && less(a_[r], a_[l])) ? r : l;
            if (!less(a_[c], v)) break;
            a_[i] = a_[c];
            i = c;
        }
        a_[i] = v;
        return ret;
    }

private:
    static bool less(const HeapItem& a, const HeapItem& b) {
        if (a.f != b.f) return a.f < b.f;
        if (a.g != b.g) return a.g > b.g; // deeper first on equal f
        return a.state < b.state;
    }
    std::vector<HeapItem> a_;
};

class SRBSolver {
public:
    void load(const std::string& inst_path,
              const std::string& port_path,
              const std::string& arc_path,
              const std::string& net_path,
              const std::string& gap_path) {
        load_ports(port_path);
        load_instances(inst_path);
        load_nets(net_path);
        load_arcs(arc_path);   // requires Net mapping for MacroEdge creation
        load_gaps(gap_path);
        precompute_line_prefix();
        precompute_spatial();
        precompute_heuristic();
        build_relaxed_cell_reverse();
        build_route_reverse();
        precompute_route_port_lower_bounds();
        precompute_landmarks();
        init_query_storage();
    }

    QueryResult query_spec(const std::string& from_spec, const std::string& to_spec) {
        query_need_path_ = true;
        ensure_parent_storage();
        auto from = split_spec(from_spec);
        auto to   = split_spec(to_spec);
        return query(from.first, from.second, to.first, to.second,
                     std::numeric_limits<uint64_t>::max());
    }

    // Batch mode: same exact shortest-delay search, but skip parent writes/path reconstruction.
    QueryResult query_delay_spec(const std::string& from_spec, const std::string& to_spec) {
        return query_delay_spec_bounded(from_spec, to_spec,
                                        std::numeric_limits<uint64_t>::max());
    }

    QueryResult query_delay_spec_bounded(const std::string& from_spec,
                                         const std::string& to_spec,
                                         uint64_t max_expanded) {
        query_need_path_ = false;
        auto from = split_spec(from_spec);
        auto to   = split_spec(to_spec);
        return query(from.first, from.second, to.first, to.second, max_expanded);
    }

    QueryResult query_delay_spec_bounded_bidirectional(const std::string& from_spec,
                                                       const std::string& to_spec,
                                                       uint64_t max_expanded) {
        query_need_path_ = false;
        auto from = split_spec(from_spec);
        auto to = split_spec(to_spec);
        return query_bidirectional(from.first, from.second, to.first, to.second, max_expanded);
    }

    // One Dijkstra traversal answers many destinations sharing an identical source.
    // A finite group budget is safe: only destinations whose shortest distance is
    // proven are returned; unresolved destinations are marked budget_exhausted so
    // an offline caller can finish them with the ordinary unbounded A* query.
    std::vector<QueryResult> query_delays_same_source_spec(
        const std::string& from_spec,
        const std::vector<std::string>& to_specs,
        uint64_t max_group_expanded = std::numeric_limits<uint64_t>::max(),
        bool include_paths = false) {
        query_need_path_ = include_paths;
        if (include_paths) ensure_parent_storage();
        const auto from = split_spec(from_spec);
        const auto source_cell_it = inst_name_to_cell_.find(from.first);
        const auto source_port_it = port_name_to_id_.find(from.second);
        if (source_cell_it == inst_name_to_cell_.end())
            throw std::runtime_error("unknown source instance: " + from.first);
        if (source_port_it == port_name_to_id_.end())
            throw std::runtime_error("unknown source port: " + from.second);
        const int32_t source_cell = source_cell_it->second;
        const uint16_t source_pid = source_port_it->second;
        query_src_cell_ = source_cell;
        query_src_pid_ = source_pid;

        struct BatchTarget {
            int32_t cell = -1;
            uint16_t pid = NO_U16;
            int16_t route = -1;
            bool input = false;
            bool pending = true;
            uint32_t candidate = INF32;
            uint32_t terminal_state = PARENT_ROOT_INPUT;
            uint16_t terminal_out = NO_U16;
            bool direct = false;
        };
        std::vector<QueryResult> results(to_specs.size());
        std::vector<BatchTarget> targets(to_specs.size());
        std::unordered_map<int32_t, std::vector<size_t>> targets_by_cell;
        size_t pending = 0;
        for (size_t index = 0; index < to_specs.size(); ++index) {
            const auto to = split_spec(to_specs[index]);
            const auto cell_it = inst_name_to_cell_.find(to.first);
            const auto port_it = port_name_to_id_.find(to.second);
            if (cell_it == inst_name_to_cell_.end())
                throw std::runtime_error("unknown target instance: " + to.first);
            if (port_it == port_name_to_id_.end())
                throw std::runtime_error("unknown target port: " + to.second);
            BatchTarget& target = targets[index];
            target.cell = cell_it->second;
            target.pid = port_it->second;
            target.input = port_is_input_[target.pid] != 0;
            if (target.cell == source_cell && target.pid == source_pid) {
                target.pending = false;
                results[index].reachable = true;
                results[index].delay = 0;
                if (include_paths) results[index].path.push_back(from_spec);
                continue;
            }
            if (target.input) {
                const uint16_t full_input = static_cast<uint16_t>(port_to_input_[target.pid]);
                target.route = input_to_route_[full_input];
                if (target.route < 0) {
                    // Query-only Inputs can never be reached by a Net.
                    target.pending = false;
                    continue;
                }
            }
            ++pending;
            targets_by_cell[target.cell].push_back(index);
        }
        if (pending == 0) return results;

        begin_query();
        heap_.clear();
        auto relax = [&](uint32_t state, uint32_t distance,
                         uint32_t parent, uint16_t via_output) {
            if (stamp_[state] != generation_ || distance < dist_[state]) {
                stamp_[state] = generation_;
                dist_[state] = distance;
                if (include_paths) {
                    parent_state_[state] = parent;
                    parent_out_[state] = via_output;
                }
                heap_.push(HeapItem{distance, distance, state});
            }
        };
        using Candidate = std::pair<uint32_t, size_t>;
        std::priority_queue<Candidate, std::vector<Candidate>, std::greater<Candidate>> candidates;
        auto offer = [&](size_t index, uint32_t distance,
                         uint32_t terminal_state, uint16_t terminal_out,
                         bool direct) {
            BatchTarget& target = targets[index];
            if (target.pending && distance < target.candidate) {
                target.candidate = distance;
                target.terminal_state = terminal_state;
                target.terminal_out = terminal_out;
                target.direct = direct;
                candidates.push(Candidate{distance, index});
            }
        };
        auto finish_candidates = [&](uint32_t lower_bound) {
            while (!candidates.empty() && candidates.top().first <= lower_bound) {
                const auto [distance, index] = candidates.top();
                candidates.pop();
                BatchTarget& target = targets[index];
                if (!target.pending || target.candidate != distance) continue;
                target.pending = false;
                --pending;
                results[index].reachable = true;
                results[index].delay = distance;
                if (include_paths) {
                    if (target.direct) {
                        results[index].path = {from_spec, to_specs[index]};
                    } else {
                        results[index].path = reconstruct(target.terminal_state);
                        if (target.terminal_out != NO_U16)
                            results[index].path.push_back(to_specs[index]);
                    }
                }
            }
        };

        if (port_is_input_[source_pid]) {
            const uint16_t full_input = static_cast<uint16_t>(port_to_input_[source_pid]);
            const int16_t route = input_to_route_[full_input];
            if (route >= 0) {
                relax(state_id(source_cell, static_cast<uint16_t>(route)), 0,
                      PARENT_ROOT_INPUT, NO_U16);
            } else {
                const auto local = targets_by_cell.find(source_cell);
                if (local != targets_by_cell.end()) {
                    for (size_t index : local->second) {
                        if (targets[index].input) continue;
                        const uint16_t direct = arc_delay_to_output(full_input, targets[index].pid);
                        if (direct != NO_U16)
                            offer(index, direct, PARENT_ROOT_INPUT, NO_U16, true);
                    }
                }
                for (const MacroEdge& edge : transitions_[full_input]) {
                    const size_t si = spatial_index(source_cell, edge.net_id);
                    const int32_t next_cell = spatial_next_[si];
                    if (next_cell < 0) continue;
                    relax(state_id(next_cell, edge.next_input),
                          static_cast<uint32_t>(edge.arc_delay) + spatial_extra_[si],
                          PARENT_ROOT_NONROUTING_INPUT, edge.out_port);
                }
            }
        } else {
            const int16_t net = net_id_by_output_[source_pid];
            if (net >= 0) {
                const size_t si = spatial_index(source_cell, static_cast<uint16_t>(net));
                const int32_t next_cell = spatial_next_[si];
                if (next_cell >= 0) {
                    relax(state_id(next_cell, nets_[static_cast<uint16_t>(net)].dst_input),
                          spatial_extra_[si], PARENT_ROOT_OUTPUT, source_pid);
                }
            }
        }

        uint64_t expanded = 0;
        bool exhausted = false;
        while (pending) {
            while (!heap_.empty()) {
                const HeapItem& top = heap_.top();
                if (stamp_[top.state] == generation_ && dist_[top.state] == top.g) break;
                heap_.pop();
            }
            const uint32_t lower_bound = heap_.empty() ? INF32 : heap_.top().g;
            finish_candidates(lower_bound);
            if (!pending || heap_.empty()) break;
            if (expanded >= max_group_expanded) {
                exhausted = true;
                break;
            }

            const HeapItem item = heap_.pop();
            const int32_t cell = static_cast<int32_t>(item.state / routing_input_count_);
            const uint16_t route = static_cast<uint16_t>(item.state % routing_input_count_);
            const uint16_t full_input = route_to_input_[route];
            ++expanded;

            const auto local = targets_by_cell.find(cell);
            if (local != targets_by_cell.end()) {
                for (size_t index : local->second) {
                    BatchTarget& target = targets[index];
                    if (!target.pending) continue;
                    if (target.input) {
                        if (target.route == static_cast<int16_t>(route)) {
                            target.pending = false;
                            --pending;
                            results[index].reachable = true;
                            results[index].delay = item.g;
                            if (include_paths) results[index].path = reconstruct(item.state);
                        }
                    } else {
                        const uint16_t arc = arc_delay_to_output(full_input, target.pid);
                        if (arc != NO_U16)
                            offer(index, item.g + arc, item.state, target.pid, false);
                    }
                }
            }

            for (const MacroEdge& edge : transitions_[full_input]) {
                const size_t si = spatial_index(cell, edge.net_id);
                const int32_t next_cell = spatial_next_[si];
                if (next_cell < 0) continue;
                relax(state_id(next_cell, edge.next_input),
                      item.g + edge.arc_delay + spatial_extra_[si], item.state, edge.out_port);
            }
        }
        if (!exhausted) finish_candidates(INF32);
        for (size_t index = 0; index < results.size(); ++index) {
            results[index].expanded = expanded;
            if (exhausted && targets[index].pending) results[index].budget_exhausted = true;
        }
        return results;
    }

    void enable_bidirectional() {
        if (!spatial_prev_.empty()) return;
        build_bidirectional_index();
        const size_t states = static_cast<size_t>(cells_.size()) * routing_input_count_;
        reverse_dist_.assign(states, INF32);
        reverse_stamp_.assign(states, 0);
        reverse_heap_.reserve(1u << 20);
    }

    QueryResult query(const std::string& src_inst,
                      const std::string& src_port,
                      const std::string& dst_inst,
                      const std::string& dst_port,
                      uint64_t max_expanded = std::numeric_limits<uint64_t>::max()) {
        QueryResult result;

        auto sit = inst_name_to_cell_.find(src_inst);
        auto dit = inst_name_to_cell_.find(dst_inst);
        auto spit = port_name_to_id_.find(src_port);
        auto dpit = port_name_to_id_.find(dst_port);
        if (sit == inst_name_to_cell_.end()) throw std::runtime_error("unknown source instance: " + src_inst);
        if (dit == inst_name_to_cell_.end()) throw std::runtime_error("unknown target instance: " + dst_inst);
        if (spit == port_name_to_id_.end()) throw std::runtime_error("unknown source port: " + src_port);
        if (dpit == port_name_to_id_.end()) throw std::runtime_error("unknown target port: " + dst_port);

        const int32_t src_cell = sit->second;
        const int32_t dst_cell = dit->second;
        const uint16_t src_pid = spit->second;
        const uint16_t dst_pid = dpit->second;

        query_src_cell_ = src_cell;
        query_src_pid_ = src_pid;

        if (src_cell == dst_cell && src_pid == dst_pid) {
            result.reachable = true;
            result.delay = 0;
            if (query_need_path_) result.path.push_back(format_node(src_cell, src_pid));
            return result;
        }

        query_dst_cell_ = dst_cell;
        query_dst_x_ = cells_[dst_cell].x;
        query_dst_y_ = cells_[dst_cell].y;
        query_dst_pid_ = dst_pid;
        select_query_landmarks(src_cell, dst_cell);

        begin_query();
        heap_.clear();

        const bool target_is_input = port_is_input_[dst_pid] != 0;
        int target_route = -1;
        if (target_is_input) {
            uint16_t full_target_in = static_cast<uint16_t>(port_to_input_[dst_pid]);
            target_route = input_to_route_[full_target_in];
            // A query-only Input is never the destination of any Net. Since identical
            // source/target was handled above, it cannot be reached.
            if (target_route < 0) return result;
        }

        uint32_t best = INF32;
        uint32_t best_terminal_state = PARENT_ROOT_INPUT;
        uint16_t best_terminal_out = NO_U16;
        bool best_is_direct = false;

        // Seed search. Persistent states are only the 160 Inputs that are Net targets.
        if (port_is_input_[src_pid]) {
            uint16_t full_in = static_cast<uint16_t>(port_to_input_[src_pid]);
            int16_t route = input_to_route_[full_in];
            if (route >= 0) {
                relax_root_input(state_id(src_cell, static_cast<uint16_t>(route)), 0);
            } else {
                // Query-only Input: expand it once instead of allocating it at every SRB.
                if (!target_is_input && src_cell == dst_cell) {
                    uint16_t d = arc_delay_to_output(full_in, dst_pid);
                    if (d != NO_U16) { best = d; best_is_direct = true; }
                }
                for (const MacroEdge& tr : transitions_[full_in]) {
                    size_t si = spatial_index(src_cell, tr.net_id);
                    int32_t next_cell = spatial_next_[si];
                    if (next_cell < 0) continue;
                    uint32_t nd = static_cast<uint32_t>(tr.arc_delay) + spatial_extra_[si];
                    if (nd >= best) continue;
                    relax_root_nonrouting(state_id(next_cell, tr.next_input), nd, tr.out_port);
                }
            }
        } else {
            int16_t nid = net_id_by_output_[src_pid];
            if (nid >= 0) {
                size_t si = spatial_index(src_cell, static_cast<uint16_t>(nid));
                int32_t next_cell = spatial_next_[si];
                if (next_cell >= 0) {
                    uint16_t next_input = nets_[static_cast<uint16_t>(nid)].dst_input;
                    uint32_t st = state_id(next_cell, next_input);
                    relax_root_output(st, spatial_extra_[si]);
                }
            }
        }

        while (!heap_.empty()) {
            HeapItem q = heap_.pop();
            if (q.f >= best) break;
            if (stamp_[q.state] != generation_ || dist_[q.state] != q.g) continue;

            if (result.expanded >= max_expanded) {
                result.budget_exhausted = true;
                return result;
            }

            int32_t cell = static_cast<int32_t>(q.state / routing_input_count_);
            uint16_t route = static_cast<uint16_t>(q.state % routing_input_count_);
            uint16_t in = route_to_input_[route];
            ++result.expanded;

            if (target_is_input && cell == dst_cell && static_cast<int>(route) == target_route) {
                best = q.g;
                best_terminal_state = q.state;
                best_is_direct = false;
                break;
            }

            // If target is Output, finish with one internal Arc in target SRB.
            if (!target_is_input && cell == dst_cell) {
                uint16_t d = arc_delay_to_output(in, dst_pid);
                if (d != NO_U16) {
                    uint32_t cand = q.g + d;
                    if (cand < best) {
                        best = cand;
                        best_terminal_state = q.state;
                        best_terminal_out = dst_pid;
                        best_is_direct = false;
                    }
                }
            }

            // Normal compressed transitions: Input --Arc--> Output --Net--> next Input.
            const auto& trs = transitions_[in];
            for (const MacroEdge& tr : trs) {
                size_t si = spatial_index(cell, tr.net_id);
                int32_t next_cell = spatial_next_[si];
                if (next_cell < 0) continue;

                uint32_t nd = q.g + tr.arc_delay + spatial_extra_[si];
                if (nd >= best) continue;

                uint32_t ns = state_id(next_cell, tr.next_input);
                relax_from(ns, nd, q.state, tr.out_port);
            }
        }

        if (best == INF32) return result;

        result.reachable = true;
        result.delay = best;
        if (!query_need_path_) return result;
        if (best_is_direct) {
            result.path.push_back(format_node(src_cell, src_pid));
            result.path.push_back(format_node(dst_cell, dst_pid));
            return result;
        }
        result.path = reconstruct(best_terminal_state);
        if (best_terminal_out != NO_U16) {
            result.path.push_back(format_node(dst_cell, best_terminal_out));
        }
        return result;
    }

    QueryResult query_bidirectional(const std::string& src_inst,
                                    const std::string& src_port,
                                    const std::string& dst_inst,
                                    const std::string& dst_port,
                                    uint64_t max_expanded) {
        if (spatial_prev_.empty() || reverse_dist_.empty())
            throw std::runtime_error("bidirectional index is not enabled");
        QueryResult result;
        const auto sit = inst_name_to_cell_.find(src_inst);
        const auto dit = inst_name_to_cell_.find(dst_inst);
        const auto spit = port_name_to_id_.find(src_port);
        const auto dpit = port_name_to_id_.find(dst_port);
        if (sit == inst_name_to_cell_.end()) throw std::runtime_error("unknown source instance: " + src_inst);
        if (dit == inst_name_to_cell_.end()) throw std::runtime_error("unknown target instance: " + dst_inst);
        if (spit == port_name_to_id_.end()) throw std::runtime_error("unknown source port: " + src_port);
        if (dpit == port_name_to_id_.end()) throw std::runtime_error("unknown target port: " + dst_port);

        const int32_t src_cell = sit->second;
        const int32_t dst_cell = dit->second;
        const uint16_t src_pid = spit->second;
        const uint16_t dst_pid = dpit->second;
        if (src_cell == dst_cell && src_pid == dst_pid) {
            result.reachable = true;
            result.delay = 0;
            return result;
        }

        begin_query();
        heap_.clear();
        reverse_heap_.clear();
        auto relax_forward = [&](uint32_t state, uint32_t distance) {
            if (stamp_[state] != generation_ || distance < dist_[state]) {
                stamp_[state] = generation_;
                dist_[state] = distance;
                heap_.push(HeapItem{distance, distance, state});
            }
        };
        auto relax_reverse = [&](uint32_t state, uint32_t distance) {
            if (reverse_stamp_[state] != generation_ || distance < reverse_dist_[state]) {
                reverse_stamp_[state] = generation_;
                reverse_dist_[state] = distance;
                reverse_heap_.push(HeapItem{distance, distance, state});
            }
        };

        uint32_t best = INF32;
        const bool target_is_input = port_is_input_[dst_pid] != 0;
        if (target_is_input) {
            const uint16_t full_input = static_cast<uint16_t>(port_to_input_[dst_pid]);
            const int16_t route = input_to_route_[full_input];
            if (route < 0) return result;
            relax_reverse(state_id(dst_cell, static_cast<uint16_t>(route)), 0);
        } else {
            for (uint16_t route = 0; route < routing_input_count_; ++route) {
                const uint16_t full_input = route_to_input_[route];
                const uint16_t arc = arc_delay_to_output(full_input, dst_pid);
                if (arc != NO_U16) relax_reverse(state_id(dst_cell, route), arc);
            }
        }

        if (port_is_input_[src_pid]) {
            const uint16_t full_input = static_cast<uint16_t>(port_to_input_[src_pid]);
            const int16_t route = input_to_route_[full_input];
            if (route >= 0) {
                relax_forward(state_id(src_cell, static_cast<uint16_t>(route)), 0);
            } else {
                if (!target_is_input && src_cell == dst_cell) {
                    const uint16_t direct = arc_delay_to_output(full_input, dst_pid);
                    if (direct != NO_U16) best = direct;
                }
                for (const MacroEdge& edge : transitions_[full_input]) {
                    const size_t si = spatial_index(src_cell, edge.net_id);
                    const int32_t next_cell = spatial_next_[si];
                    if (next_cell < 0) continue;
                    relax_forward(state_id(next_cell, edge.next_input),
                                  static_cast<uint32_t>(edge.arc_delay) + spatial_extra_[si]);
                }
            }
        } else {
            const int16_t net = net_id_by_output_[src_pid];
            if (net >= 0) {
                const size_t si = spatial_index(src_cell, static_cast<uint16_t>(net));
                const int32_t next_cell = spatial_next_[si];
                if (next_cell >= 0) {
                    relax_forward(state_id(next_cell, nets_[static_cast<uint16_t>(net)].dst_input),
                                  spatial_extra_[si]);
                }
            }
        }

        auto clean_forward = [&]() {
            while (!heap_.empty()) {
                const HeapItem& q = heap_.top();
                if (stamp_[q.state] == generation_ && dist_[q.state] == q.g) break;
                heap_.pop();
            }
        };
        auto clean_reverse = [&]() {
            while (!reverse_heap_.empty()) {
                const HeapItem& q = reverse_heap_.top();
                if (reverse_stamp_[q.state] == generation_ && reverse_dist_[q.state] == q.g) break;
                reverse_heap_.pop();
            }
        };

        while (true) {
            clean_forward();
            clean_reverse();
            if (heap_.empty() || reverse_heap_.empty()) break;
            const uint64_t lower = static_cast<uint64_t>(heap_.top().g) + reverse_heap_.top().g;
            if (best != INF32 && lower >= best) break;
            if (result.expanded >= max_expanded) {
                result.budget_exhausted = true;
                return result;
            }

            if (heap_.top().g <= reverse_heap_.top().g) {
                const HeapItem q = heap_.pop();
                ++result.expanded;
                if (reverse_stamp_[q.state] == generation_) {
                    best = std::min(best, q.g + reverse_dist_[q.state]);
                }
                const int32_t cell = static_cast<int32_t>(q.state / routing_input_count_);
                const uint16_t route = static_cast<uint16_t>(q.state % routing_input_count_);
                const uint16_t full_input = route_to_input_[route];
                for (const MacroEdge& edge : transitions_[full_input]) {
                    const size_t si = spatial_index(cell, edge.net_id);
                    const int32_t next_cell = spatial_next_[si];
                    if (next_cell < 0) continue;
                    const uint32_t next_state = state_id(next_cell, edge.next_input);
                    const uint32_t next_distance = q.g + edge.arc_delay + spatial_extra_[si];
                    relax_forward(next_state, next_distance);
                    if (reverse_stamp_[next_state] == generation_)
                        best = std::min(best, next_distance + reverse_dist_[next_state]);
                }
            } else {
                const HeapItem q = reverse_heap_.pop();
                ++result.expanded;
                if (stamp_[q.state] == generation_) {
                    best = std::min(best, q.g + dist_[q.state]);
                }
                const int32_t cell = static_cast<int32_t>(q.state / routing_input_count_);
                const uint16_t route = static_cast<uint16_t>(q.state % routing_input_count_);
                for (const ReverseMacroEdge& edge : reverse_transitions_[route]) {
                    const size_t ri = static_cast<size_t>(cell) * nets_.size() + edge.net_id;
                    const int32_t previous_cell = spatial_prev_[ri];
                    if (previous_cell < 0) continue;
                    const size_t forward_si = spatial_index(previous_cell, edge.net_id);
                    const uint32_t previous_state = state_id(previous_cell, edge.source_route);
                    const uint32_t previous_distance = q.g + edge.arc_delay + spatial_extra_[forward_si];
                    relax_reverse(previous_state, previous_distance);
                    if (stamp_[previous_state] == generation_)
                        best = std::min(best, previous_distance + dist_[previous_state]);
                }
            }
        }

        if (best != INF32) {
            result.reachable = true;
            result.delay = best;
        }
        return result;
    }

    void save_binary(const std::string& path) const {
        std::ofstream out(path, std::ios::binary);
        if (!out) throw std::runtime_error("cannot create binary graph: " + path);

        const char magic[8] = {'S','R','B','G','R','P','H','1'};
        out.write(magic, sizeof(magic));
        write_scalar(out, static_cast<uint32_t>(1)); // format version
        write_scalar(out, static_cast<uint32_t>(0x01020304)); // endian marker

        write_scalar(out, static_cast<int32_t>(width_));
        write_scalar(out, static_cast<int32_t>(height_));
        write_scalar(out, input_count_);
        write_scalar(out, routing_input_count_);
        write_scalar(out, static_cast<uint64_t>(arc_count_));

        write_pod_vector(out, cells_);
        write_string_vector(out, port_names_);
        write_pod_vector(out, port_is_input_);
        write_pod_vector(out, port_to_input_);
        write_pod_vector(out, input_to_route_);
        write_pod_vector(out, route_to_input_);
        write_pod_vector(out, nets_);
        write_pod_vector(out, net_id_by_output_);

        write_scalar(out, static_cast<uint64_t>(transitions_.size()));
        for (const auto& v : transitions_) write_pod_vector(out, v);

        write_pod_vector(out, arc_to_output_);
        write_pod_vector(out, spatial_next_);
        write_pod_vector(out, spatial_extra_);
        write_pod_vector(out, cell_rev_off_);
        write_pod_vector(out, cell_rev_src_);
        write_pod_vector(out, cell_rev_cost_);

        // Keep the original instance-name mapping so query syntax remains exactly compatible.
        write_scalar(out, static_cast<uint64_t>(inst_name_to_cell_.size()));
        for (const auto& kv : inst_name_to_cell_) {
            write_string(out, kv.first);
            write_scalar(out, kv.second);
        }

        out.flush();
        if (!out) throw std::runtime_error("failed while writing binary graph: " + path);
    }

    void load_binary(const std::string& path, bool prepare_cell_heuristics = true) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("cannot open binary graph: " + path);
        load_binary_stream(in, path, prepare_cell_heuristics);
    }

    void load_embedded_binary(const std::string& executable_path,
                              bool prepare_cell_heuristics = true) {
        std::ifstream in(executable_path, std::ios::binary);
        if (!in) throw std::runtime_error("cannot open executable for embedded graph: " + executable_path);
        in.seekg(0, std::ios::end);
        const std::streamoff end = in.tellg();
        if (end < 16) throw std::runtime_error("executable has no V9 graph footer");
        in.seekg(end - static_cast<std::streamoff>(16));
        const uint64_t graph_size = read_scalar<uint64_t>(in);
        char footer_magic[8] = {};
        in.read(footer_magic, sizeof(footer_magic));
        const char expected_footer[8] = {'V','9','G','R','A','P','H','1'};
        if (!in || !std::equal(std::begin(footer_magic), std::end(footer_magic),
                               std::begin(expected_footer)))
            throw std::runtime_error("executable has no valid V9 embedded graph footer");
        if (graph_size > static_cast<uint64_t>(end - 16))
            throw std::runtime_error("embedded V9 graph size is corrupt");
        const std::streamoff graph_offset = end - static_cast<std::streamoff>(16) -
                                            static_cast<std::streamoff>(graph_size);
        in.seekg(graph_offset);
        load_binary_stream(in, executable_path + " (embedded graph)", prepare_cell_heuristics);
    }

    void load_binary_stream(std::istream& in, const std::string& path,
                            bool prepare_cell_heuristics) {
        clear_all();

        char magic[8] = {};
        in.read(magic, sizeof(magic));
        const char expected[8] = {'S','R','B','G','R','P','H','1'};
        if (!in || !std::equal(std::begin(magic), std::end(magic), std::begin(expected))) {
            throw std::runtime_error("bad srb_graph.bin magic/version");
        }
        uint32_t version = read_scalar<uint32_t>(in);
        uint32_t endian = read_scalar<uint32_t>(in);
        if (version != 1) throw std::runtime_error("unsupported binary graph version: " + std::to_string(version));
        if (endian != 0x01020304) throw std::runtime_error("binary graph endian mismatch");

        width_ = read_scalar<int32_t>(in);
        height_ = read_scalar<int32_t>(in);
        input_count_ = read_scalar<uint16_t>(in);
        routing_input_count_ = read_scalar<uint16_t>(in);
        arc_count_ = static_cast<size_t>(read_scalar<uint64_t>(in));

        read_pod_vector(in, cells_);
        read_string_vector(in, port_names_);
        read_pod_vector(in, port_is_input_);
        read_pod_vector(in, port_to_input_);
        read_pod_vector(in, input_to_route_);
        read_pod_vector(in, route_to_input_);
        read_pod_vector(in, nets_);
        read_pod_vector(in, net_id_by_output_);

        uint64_t trans_n = read_scalar<uint64_t>(in);
        if (trans_n > 1000000ull) throw std::runtime_error("corrupt transition count in binary graph");
        transitions_.assign(static_cast<size_t>(trans_n), {});
        for (auto& v : transitions_) read_pod_vector(in, v);

        read_pod_vector(in, arc_to_output_);
        read_pod_vector(in, spatial_next_);
        read_pod_vector(in, spatial_extra_);
        read_pod_vector(in, cell_rev_off_);
        read_pod_vector(in, cell_rev_src_);
        read_pod_vector(in, cell_rev_cost_);

        uint64_t inst_n = read_scalar<uint64_t>(in);
        if (inst_n > 10000000ull) throw std::runtime_error("corrupt instance count in binary graph");
        inst_name_to_cell_.reserve(static_cast<size_t>(inst_n * 2));
        for (uint64_t i = 0; i < inst_n; ++i) {
            std::string name = read_string(in);
            int32_t cell = read_scalar<int32_t>(in);
            inst_name_to_cell_.emplace(std::move(name), cell);
        }
        if (!in) throw std::runtime_error("truncated/corrupt binary graph: " + path);

        // Rebuild tiny indexes that are cheaper than serializing and are independent of Gap geometry.
        port_name_to_id_.reserve(port_names_.size() * 2);
        for (uint16_t pid = 0; pid < port_names_.size(); ++pid) port_name_to_id_[port_names_[pid]] = pid;

        coord_to_cell_.assign(static_cast<size_t>(width_) * height_, -1);
        for (int32_t cell = 0; cell < static_cast<int32_t>(cells_.size()); ++cell) {
            const Coord& c = cells_[cell];
            if (c.x < 0 || c.x >= width_ || c.y < 0 || c.y >= height_) throw std::runtime_error("corrupt cell coordinate in binary graph");
            coord_to_cell_[coord_index(c.x, c.y)] = cell;
        }

        // V9 deliberately rebuilds compact reusable heuristics once at startup.
        // No query performs a full reverse traversal of the 60,200-cell graph.
        if (prepare_cell_heuristics) precompute_heuristic();
        build_route_reverse();
        precompute_route_port_lower_bounds();
        if (prepare_cell_heuristics) precompute_landmarks();
        query_cell_h_.resize(cells_.size()); // retained for v1 graph compatibility only
        init_query_storage();
        validate_binary_sizes();
    }

    void print_stats() const {
        size_t transition_count = 0;
        for (const auto& v : transitions_) transition_count += v.size();
        uint64_t states = static_cast<uint64_t>(cells_.size()) * routing_input_count_;
        uint64_t spatial_items = static_cast<uint64_t>(cells_.size()) * nets_.size();

        std::cerr << "cells=" << cells_.size()
                  << " grid=" << width_ << "x" << height_
                  << " ports=" << port_names_.size()
                  << " inputs=" << input_count_
                  << " routing_inputs=" << routing_input_count_
                  << " arcs=" << arc_count_
                  << " nets=" << nets_.size()
                  << " macro_edges=" << transition_count
                  << " states=" << states
                  << " spatial_items=" << spatial_items
                  << "\n";

        double mib = 0.0;
        mib += dist_.size() * sizeof(uint32_t);
        mib += stamp_.size() * sizeof(uint32_t);
        mib += parent_state_.size() * sizeof(uint32_t);
        mib += parent_out_.size() * sizeof(uint16_t);
        mib += spatial_next_.size() * sizeof(int32_t);
        mib += spatial_extra_.size() * sizeof(uint16_t);
        mib += h_east_.size() * sizeof(uint32_t);
        mib += h_west_.size() * sizeof(uint32_t);
        mib += h_north_.size() * sizeof(uint32_t);
        mib += h_south_.size() * sizeof(uint32_t);
        mib += cell_rev_off_.size() * sizeof(uint32_t);
        mib += cell_rev_src_.size() * sizeof(uint32_t);
        mib += cell_rev_cost_.size() * sizeof(uint16_t);
        mib += query_cell_h_.size() * sizeof(uint32_t);
        mib += landmark_from_.size() * sizeof(uint32_t);
        mib += landmark_to_.size() * sizeof(uint32_t);
        mib /= 1048576.0;
        std::cerr << "major persistent arrays ~= " << mib << " MiB\n";

        std::unordered_map<std::string, uint32_t> target_signatures;
        std::unordered_map<std::string, uint32_t> source_signatures;
        for (uint16_t pid = 0; pid < port_names_.size(); ++pid) {
            std::string target_key;
            std::string source_key;
            if (port_is_input_[pid]) {
                const uint16_t input = static_cast<uint16_t>(port_to_input_[pid]);
                const int16_t route = input_to_route_[input];
                target_key = "I" + std::to_string(route);
                if (route >= 0) {
                    source_key = "I" + std::to_string(route);
                } else {
                    source_key = "Q";
                    for (const MacroEdge& edge : transitions_[input]) {
                        source_key += std::to_string(edge.out_port) + ":" +
                                      std::to_string(edge.next_input) + ":" +
                                      std::to_string(edge.arc_delay) + ":" +
                                      std::to_string(edge.net_id) + ";";
                    }
                }
            } else {
                target_key = "O";
                for (uint16_t route = 0; route < routing_input_count_; ++route) {
                    target_key += std::to_string(arc_delay_to_output(route_to_input_[route], pid)) + ",";
                }
                source_key = "O" + std::to_string(net_id_by_output_[pid]);
            }
            ++target_signatures[target_key];
            ++source_signatures[source_key];
        }
        std::cerr << "port structural signatures: source=" << source_signatures.size()
                  << " target=" << target_signatures.size() << "\n";
    }

private:
    template <class T>
    static void write_scalar(std::ostream& out, const T& value) {
        static_assert(std::is_trivially_copyable<T>::value, "binary scalar must be trivially copyable");
        out.write(reinterpret_cast<const char*>(&value), sizeof(T));
        if (!out) throw std::runtime_error("binary graph write failed");
    }

    template <class T>
    static T read_scalar(std::istream& in) {
        static_assert(std::is_trivially_copyable<T>::value, "binary scalar must be trivially copyable");
        T value{};
        in.read(reinterpret_cast<char*>(&value), sizeof(T));
        if (!in) throw std::runtime_error("binary graph read failed");
        return value;
    }

    template <class T>
    static void write_pod_vector(std::ostream& out, const std::vector<T>& v) {
        static_assert(std::is_trivially_copyable<T>::value, "binary vector element must be trivially copyable");
        write_scalar(out, static_cast<uint64_t>(v.size()));
        if (!v.empty()) out.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(v.size() * sizeof(T)));
        if (!out) throw std::runtime_error("binary graph vector write failed");
    }

    template <class T>
    static void read_pod_vector(std::istream& in, std::vector<T>& v) {
        static_assert(std::is_trivially_copyable<T>::value, "binary vector element must be trivially copyable");
        uint64_t n = read_scalar<uint64_t>(in);
        if (n > (1ull << 34)) throw std::runtime_error("corrupt binary vector size");
        v.resize(static_cast<size_t>(n));
        if (!v.empty()) in.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(v.size() * sizeof(T)));
        if (!in) throw std::runtime_error("binary graph vector read failed");
    }

    static void write_string(std::ostream& out, const std::string& s) {
        write_scalar(out, static_cast<uint32_t>(s.size()));
        if (!s.empty()) out.write(s.data(), static_cast<std::streamsize>(s.size()));
        if (!out) throw std::runtime_error("binary graph string write failed");
    }

    static std::string read_string(std::istream& in) {
        uint32_t n = read_scalar<uint32_t>(in);
        if (n > (1u << 20)) throw std::runtime_error("corrupt binary string length");
        std::string s(n, '\0');
        if (n) in.read(&s[0], n);
        if (!in) throw std::runtime_error("binary graph string read failed");
        return s;
    }

    static void write_string_vector(std::ostream& out, const std::vector<std::string>& v) {
        write_scalar(out, static_cast<uint64_t>(v.size()));
        for (const auto& s : v) write_string(out, s);
    }

    static void read_string_vector(std::istream& in, std::vector<std::string>& v) {
        uint64_t n = read_scalar<uint64_t>(in);
        if (n > 10000000ull) throw std::runtime_error("corrupt binary string-vector count");
        v.resize(static_cast<size_t>(n));
        for (auto& s : v) s = read_string(in);
    }

    void clear_all() {
        *this = SRBSolver();
    }

    void validate_binary_sizes() const {
        if (width_ <= 0 || height_ <= 0 || cells_.empty() || port_names_.empty() || nets_.empty())
            throw std::runtime_error("binary graph has invalid empty metadata");
        if (port_names_.size() != port_is_input_.size() || port_names_.size() != port_to_input_.size())
            throw std::runtime_error("binary graph port arrays mismatch");
        if (route_to_input_.size() != routing_input_count_ || transitions_.size() != input_count_)
            throw std::runtime_error("binary graph routing arrays mismatch");
        const size_t spatial_n = cells_.size() * nets_.size();
        if (spatial_next_.size() != spatial_n || spatial_extra_.size() != spatial_n)
            throw std::runtime_error("binary graph spatial arrays mismatch");
        if (cell_rev_off_.size() != cells_.size() + 1 || cell_rev_src_.size() != cell_rev_cost_.size())
            throw std::runtime_error("binary graph cell reverse graph mismatch");
    }

    // Grid / instances
    int width_ = 0;
    int height_ = 0;
    std::vector<Coord> cells_;
    std::vector<int32_t> coord_to_cell_;
    std::unordered_map<std::string, int32_t> inst_name_to_cell_;

    // Ports
    std::vector<std::string> port_names_;
    std::unordered_map<std::string, uint16_t> port_name_to_id_;
    std::vector<uint8_t> port_is_input_;
    std::vector<int16_t> port_to_input_;
    uint16_t input_count_ = 0;
    uint16_t routing_input_count_ = 0;
    std::vector<int16_t> input_to_route_;    // full compressed Input -> routing state id, -1 if query-only
    std::vector<uint16_t> route_to_input_;   // routing state id -> full compressed Input

    // Arc + Net templates
    size_t arc_count_ = 0;
    std::vector<NetRule> nets_;
    std::vector<int16_t> net_id_by_output_;
    std::vector<std::vector<MacroEdge>> transitions_;
    std::vector<uint16_t> arc_to_output_; // [input][global port], NO_U16 if absent

    // Gap model
    std::vector<GapLine> gap_lines_;
    std::vector<Block> blocks_;
    std::vector<int16_t> block_at_coord_; // -1 if no Block
    std::vector<uint32_t> x_line_prefix_; // prefix across vertical line gaps
    std::vector<uint32_t> y_line_prefix_; // prefix across horizontal line gaps

    // Precomputed position-dependent part of every Net.
    // [cell][net_id] -> destination cell + Gap extra delay; next=-1 means invalid.
    std::vector<int32_t> spatial_next_;
    std::vector<uint16_t> spatial_extra_;
    std::vector<int32_t> spatial_prev_; // [destination cell][net] -> unique source cell
    std::vector<std::vector<ReverseMacroEdge>> reverse_transitions_;

    // Admissible coordinate-only A* lower bounds.
    std::vector<uint32_t> h_east_, h_west_, h_north_, h_south_;
    std::vector<uint16_t> min_arc_by_net_;
    std::vector<GeomMove> exact_geom_moves_;
    std::vector<uint32_t> cell_rev_off_;
    std::vector<uint32_t> cell_rev_src_;
    std::vector<uint16_t> cell_rev_cost_;
    std::vector<uint32_t> query_cell_h_;
    std::vector<std::vector<std::pair<uint16_t,uint16_t>>> route_rev_;
    std::vector<uint32_t> query_route_h_;
    std::vector<uint32_t> port_route_h_; // [target port][routing state]

    // ALT landmarks on a relaxed cell-only graph. Each relaxed edge forgets the
    // current Input port and uses the cheapest Arc capable of a geometric Net move,
    // therefore all landmark distances are lower bounds for the true port-level graph.
    std::vector<GeomMove> geom_moves_;
    std::vector<int32_t> landmark_cells_;
    std::vector<uint32_t> landmark_from_; // [L][cell] = d(L, cell)
    std::vector<uint32_t> landmark_to_;   // [L][cell] = d(cell, L)
    static constexpr int ACTIVE_LM = 8;
    int active_lm_count_ = 0;
    int active_lm_[ACTIVE_LM] = {0,0,0,0};
    uint32_t active_from_target_[ACTIVE_LM] = {0,0,0,0};
    uint32_t active_to_target_[ACTIVE_LM] = {0,0,0,0};

    int16_t query_dst_x_ = 0;
    int16_t query_dst_y_ = 0;
    int32_t query_dst_cell_ = -1;
    uint16_t query_dst_pid_ = NO_U16;

    // A* reusable arrays.
    std::vector<uint32_t> dist_;
    std::vector<uint32_t> stamp_;
    std::vector<uint32_t> parent_state_;
    std::vector<uint16_t> parent_out_;
    std::vector<uint32_t> reverse_dist_;
    std::vector<uint32_t> reverse_stamp_;
    uint32_t generation_ = 0;
    MinHeap heap_;
    MinHeap reverse_heap_;

    // Query root info for path reconstruction from an Output source.
    int32_t query_src_cell_ = -1;
    uint16_t query_src_pid_ = NO_U16;
    bool query_need_path_ = true;

    static std::pair<std::string, std::string> split_spec(const std::string& spec) {
        size_t p = spec.find('/');
        if (p == std::string::npos || p == 0 || p + 1 >= spec.size()) {
            throw std::runtime_error("bad endpoint, expected SRB_x_y/PORT: " + spec);
        }
        return {spec.substr(0, p), spec.substr(p + 1)};
    }

    size_t coord_index(int x, int y) const {
        return static_cast<size_t>(y) * static_cast<size_t>(width_) + static_cast<size_t>(x);
    }

    int32_t cell_at(int x, int y) const {
        if (x < 0 || x >= width_ || y < 0 || y >= height_) return -1;
        return coord_to_cell_[coord_index(x, y)];
    }

    int16_t block_at(int x, int y) const {
        if (x < 0 || x >= width_ || y < 0 || y >= height_) return -1;
        return block_at_coord_[coord_index(x, y)];
    }

    uint32_t state_id(int32_t cell, uint16_t input) const {
        return static_cast<uint32_t>(static_cast<uint64_t>(cell) * routing_input_count_ + input);
    }

    size_t spatial_index(int32_t cell, uint16_t net_id) const {
        return static_cast<size_t>(cell) * nets_.size() + net_id;
    }

    std::string format_node(int32_t cell, uint16_t pid) const {
        const Coord& c = cells_[cell];
        return "SRB_" + std::to_string(c.x) + "_" + std::to_string(c.y) + "/" + port_names_[pid];
    }

    uint16_t arc_delay_to_output(uint16_t in, uint16_t out_pid) const {
        return arc_to_output_[static_cast<size_t>(in) * port_names_.size() + out_pid];
    }

    void load_ports(const std::string& path) {
        Json root = load_json(path);
        const auto& arr = root.at("Port").a;
        if (arr.size() > std::numeric_limits<uint16_t>::max()) throw std::runtime_error("too many ports");

        port_names_.reserve(arr.size());
        port_is_input_.reserve(arr.size());
        port_to_input_.assign(arr.size(), -1);
        port_name_to_id_.reserve(arr.size() * 2);

        for (const Json& o : arr) {
            std::string name = sget(o, "Name");
            std::string dir  = sget(o, "Direction");
            uint16_t pid = static_cast<uint16_t>(port_names_.size());
            if (!port_name_to_id_.emplace(name, pid).second) {
                throw std::runtime_error("duplicate Port: " + name);
            }
            port_names_.push_back(std::move(name));
            if (dir == "Input") port_is_input_.push_back(1);
            else if (dir == "Output") port_is_input_.push_back(0);
            else throw std::runtime_error("unknown Port Direction: " + dir);
        }

        for (uint16_t pid = 0; pid < port_names_.size(); ++pid) {
            if (port_is_input_[pid]) port_to_input_[pid] = static_cast<int16_t>(input_count_++);
        }

        transitions_.assign(input_count_, {});
        input_to_route_.assign(input_count_, -1);
        net_id_by_output_.assign(port_names_.size(), -1);
        arc_to_output_.assign(static_cast<size_t>(input_count_) * port_names_.size(), NO_U16);
    }

    void load_instances(const std::string& path) {
        Json root = load_json(path);
        const auto& arr = root.at("Inst").a;
        int maxx = -1, maxy = -1;
        for (const Json& o : arr) {
            maxx = std::max(maxx, iget(o, "x"));
            maxy = std::max(maxy, iget(o, "y"));
        }
        width_ = maxx + 1;
        height_ = maxy + 1;
        coord_to_cell_.assign(static_cast<size_t>(width_) * height_, -1);
        cells_.reserve(arr.size());
        inst_name_to_cell_.reserve(arr.size() * 2);

        for (const Json& o : arr) {
            int x = iget(o, "x");
            int y = iget(o, "y");
            std::string name = sget(o, "name");
            if (x < 0 || x >= width_ || y < 0 || y >= height_) throw std::runtime_error("bad instance coordinate");
            int32_t id = static_cast<int32_t>(cells_.size());
            if (coord_to_cell_[coord_index(x, y)] != -1) throw std::runtime_error("duplicate instance coordinate");
            cells_.push_back(Coord{static_cast<int16_t>(x), static_cast<int16_t>(y)});
            coord_to_cell_[coord_index(x, y)] = id;
            inst_name_to_cell_[std::move(name)] = id;
        }
    }

    void load_nets(const std::string& path) {
        Json root = load_json(path);
        const auto& arr = root.at("Nets").a;
        if (arr.size() > std::numeric_limits<uint16_t>::max()) throw std::runtime_error("too many Nets");
        nets_.reserve(arr.size());

        for (const Json& o : arr) {
            std::string from = sget(o, "from");
            std::string to   = sget(o, "to");
            auto fi = port_name_to_id_.find(from);
            auto ti = port_name_to_id_.find(to);
            if (fi == port_name_to_id_.end() || ti == port_name_to_id_.end()) {
                throw std::runtime_error("Net references unknown port: " + from + " -> " + to);
            }
            uint16_t fp = fi->second;
            uint16_t tp = ti->second;
            if (port_is_input_[fp] || !port_is_input_[tp]) {
                throw std::runtime_error("expected Net Output -> Input: " + from + " -> " + to);
            }
            if (net_id_by_output_[fp] >= 0) throw std::runtime_error("duplicate Net source output: " + from);

            int dx = iget(o, "delta x");
            int dy = iget(o, "delta y");
            if ((dx == 0) == (dy == 0)) throw std::runtime_error("Net must move on exactly one axis: " + from);

            uint16_t full_in = static_cast<uint16_t>(port_to_input_[tp]);
            if (input_to_route_[full_in] < 0) {
                input_to_route_[full_in] = static_cast<int16_t>(routing_input_count_++);
                route_to_input_.push_back(full_in);
            }
            uint16_t route_in = static_cast<uint16_t>(input_to_route_[full_in]);

            uint16_t nid = static_cast<uint16_t>(nets_.size());
            nets_.push_back(NetRule{
                fp,
                route_in,
                static_cast<int16_t>(dx),
                static_cast<int16_t>(dy)
            });
            net_id_by_output_[fp] = static_cast<int16_t>(nid);
        }
    }

    void load_arcs(const std::string& path) {
        Json root = load_json(path);
        const auto& arr = root.at("Arcs").a;
        arc_count_ = arr.size();

        for (const Json& o : arr) {
            std::string from = sget(o, "from");
            std::string to   = sget(o, "to");
            int delay_i = iget(o, "delay");
            if (delay_i < 0 || delay_i >= static_cast<int>(NO_U16)) throw std::runtime_error("Arc delay out of uint16 range");

            auto fi = port_name_to_id_.find(from);
            auto ti = port_name_to_id_.find(to);
            if (fi == port_name_to_id_.end() || ti == port_name_to_id_.end()) {
                throw std::runtime_error("Arc references unknown port: " + from + " -> " + to);
            }
            uint16_t fp = fi->second;
            uint16_t tp = ti->second;
            if (!port_is_input_[fp] || port_is_input_[tp]) {
                throw std::runtime_error("expected Arc Input -> Output: " + from + " -> " + to);
            }

            uint16_t in = static_cast<uint16_t>(port_to_input_[fp]);
            uint16_t d = static_cast<uint16_t>(delay_i);
            size_t mi = static_cast<size_t>(in) * port_names_.size() + tp;
            if (d < arc_to_output_[mi]) arc_to_output_[mi] = d;

            int16_t nid = net_id_by_output_[tp];
            if (nid >= 0) {
                const NetRule& nr = nets_[static_cast<uint16_t>(nid)];
                transitions_[in].push_back(MacroEdge{
                    tp,
                    nr.dst_input,
                    d,
                    static_cast<uint16_t>(nid)
                });
            }
        }
    }

    void load_gaps(const std::string& path) {
        Json root = load_json(path);
        const Json& gap = root.at("Gap");

        for (const Json& o : gap.at("Line").a) {
            std::string direction = sget(o, "direction");
            int site = iget(o, "site");
            int delay = iget(o, "delay");
            if (delay < 0 || delay >= static_cast<int>(NO_U16)) throw std::runtime_error("Line Gap delay too large");
            if (direction == "vertical") {
                gap_lines_.push_back(GapLine{true, static_cast<int16_t>(site), static_cast<uint16_t>(delay)});
            } else if (direction == "horizontal") {
                gap_lines_.push_back(GapLine{false, static_cast<int16_t>(site), static_cast<uint16_t>(delay)});
            } else {
                throw std::runtime_error("unknown Gap Line direction: " + direction);
            }
        }

        for (const Json& o : gap.at("Block").a) {
            Block b;
            b.lower = static_cast<int16_t>(iget(o, "lower"));
            b.upper = static_cast<int16_t>(iget(o, "upper"));
            b.left  = static_cast<int16_t>(iget(o, "left"));
            b.right = static_cast<int16_t>(iget(o, "right"));
            b.vertical_crossable = bget(o, "vertical crossable");
            b.vertical_cross_delay = static_cast<uint16_t>(iget(o, "vertical cross delay"));
            b.horizontal_crossable = bget(o, "horizontal crossable");
            b.horizontal_cross_delay = static_cast<uint16_t>(iget(o, "horizontal cross delay"));
            blocks_.push_back(b);
        }

        block_at_coord_.assign(static_cast<size_t>(width_) * height_, -1);
        for (size_t bi = 0; bi < blocks_.size(); ++bi) {
            const Block& b = blocks_[bi];
            for (int y = std::max<int>(0, b.lower); y <= std::min<int>(height_ - 1, b.upper); ++y) {
                for (int x = std::max<int>(0, b.left); x <= std::min<int>(width_ - 1, b.right); ++x) {
                    block_at_coord_[coord_index(x, y)] = static_cast<int16_t>(bi);
                }
            }
        }
    }

    void precompute_line_prefix() {
        // x_line_prefix_[k] = total vertical-line penalty for boundaries with site < k.
        // Crossing x1 -> x2 costs prefix[max] - prefix[min].
        std::vector<uint32_t> x_boundary(static_cast<size_t>(width_), 0);
        std::vector<uint32_t> y_boundary(static_cast<size_t>(height_), 0);
        for (const GapLine& g : gap_lines_) {
            if (g.vertical) {
                if (g.site >= 0 && g.site < width_ - 1) x_boundary[static_cast<size_t>(g.site)] += g.delay;
            } else {
                if (g.site >= 0 && g.site < height_ - 1) y_boundary[static_cast<size_t>(g.site)] += g.delay;
            }
        }

        x_line_prefix_.assign(static_cast<size_t>(width_) + 1, 0);
        for (int x = 0; x < width_; ++x) x_line_prefix_[x + 1] = x_line_prefix_[x] + x_boundary[x];
        y_line_prefix_.assign(static_cast<size_t>(height_) + 1, 0);
        for (int y = 0; y < height_; ++y) y_line_prefix_[y + 1] = y_line_prefix_[y] + y_boundary[y];
    }

    uint32_t line_penalty(int x1, int y1, int x2, int y2) const {
        if (y1 == y2) {
            int lo = std::min(x1, x2), hi = std::max(x1, x2);
            // boundaries sites lo ... hi-1
            return x_line_prefix_[hi] - x_line_prefix_[lo];
        }
        if (x1 == x2) {
            int lo = std::min(y1, y2), hi = std::max(y1, y2);
            return y_line_prefix_[hi] - y_line_prefix_[lo];
        }
        throw std::runtime_error("Net is not axis aligned");
    }

    // Implements the semantics specified by the examples supplied by the user:
    // 1) delta counts normal SRB sites.
    // 2) when a missing site is inside a Block that IS crossable in the movement direction,
    //    that Block site does NOT consume delta; crossing the Block adds its cross delay once.
    // 3) when the Block is NOT crossable, its missing sites DO consume delta. If the requested
    //    delta lands inside such a missing site, the Net is disconnected. A longer Net may have
    //    its final counted site beyond the Block; in that case the Block adds no cross delay.
    // 4) Line Gap penalty is based on the actual source-to-final-coordinate segment and stacks
    //    with any crossable Block penalty.
    bool compute_spatial_move(int32_t src_cell,
                              const NetRule& nr,
                              int32_t& dst_cell,
                              uint32_t& extra) const {
        const Coord& s = cells_[src_cell];
        const bool horizontal = nr.dx != 0;
        const int step = horizontal ? (nr.dx > 0 ? 1 : -1) : (nr.dy > 0 ? 1 : -1);
        const int need = std::abs(horizontal ? static_cast<int>(nr.dx) : static_cast<int>(nr.dy));

        int x = s.x;
        int y = s.y;
        int counted = 0;
        uint32_t block_extra = 0;
        std::vector<uint8_t> charged(blocks_.size(), 0);

        // Safety limit is just a guard against malformed data.
        int guard = 0;
        const int guard_limit = (width_ + height_) * 4 + need + 16;

        while (counted < need) {
            if (++guard > guard_limit) throw std::runtime_error("spatial move guard triggered");
            if (horizontal) x += step;
            else y += step;

            if (x < 0 || x >= width_ || y < 0 || y >= height_) return false;

            int32_t c = cell_at(x, y);
            if (c >= 0) {
                ++counted;
                continue;
            }

            int16_t bi = block_at(x, y);
            bool crossable = false;
            uint16_t cross_delay = 0;
            if (bi >= 0) {
                const Block& b = blocks_[static_cast<size_t>(bi)];
                if (horizontal) {
                    crossable = b.horizontal_crossable;
                    cross_delay = b.horizontal_cross_delay;
                } else {
                    crossable = b.vertical_crossable;
                    cross_delay = b.vertical_cross_delay;
                }
            }

            if (crossable) {
                // Missing SRBs in a crossable Block are not counted in Net offset.
                if (!charged[static_cast<size_t>(bi)]) {
                    charged[static_cast<size_t>(bi)] = 1;
                    block_extra += cross_delay;
                }
            } else {
                // Missing SRB still consumes one unit of offset for a non-crossable Block.
                ++counted;
            }
        }

        dst_cell = cell_at(x, y);
        if (dst_cell < 0) return false; // requested offset landed inside a non-crossable/missing site

        extra = block_extra + line_penalty(s.x, s.y, x, y);
        return true;
    }

    void precompute_spatial() {
        const uint64_t n = static_cast<uint64_t>(cells_.size()) * nets_.size();
        if (n > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) throw std::runtime_error("spatial table too large");
        spatial_next_.assign(static_cast<size_t>(n), -1);
        spatial_extra_.assign(static_cast<size_t>(n), 0);

        uint32_t max_extra = 0;
        uint64_t valid = 0;
        for (int32_t cell = 0; cell < static_cast<int32_t>(cells_.size()); ++cell) {
            for (uint16_t nid = 0; nid < nets_.size(); ++nid) {
                int32_t dst = -1;
                uint32_t extra = 0;
                if (!compute_spatial_move(cell, nets_[nid], dst, extra)) continue;
                if (extra >= NO_U16) throw std::runtime_error("spatial extra delay exceeds uint16_t");
                size_t si = spatial_index(cell, nid);
                spatial_next_[si] = dst;
                spatial_extra_[si] = static_cast<uint16_t>(extra);
                max_extra = std::max(max_extra, extra);
                ++valid;
            }
        }
        std::cerr << "spatial precompute: valid=" << valid
                  << "/" << n << " max_extra=" << max_extra << "\n";
    }

    static std::vector<uint32_t> build_direction_lb(const std::vector<uint32_t>& step_cost,
                                                         int max_distance) {
        int max_step = 0;
        for (int p = 1; p < static_cast<int>(step_cost.size()); ++p) {
            if (step_cost[p] < INF32) max_step = p;
        }
        std::vector<uint32_t> h(static_cast<size_t>(max_distance) + 1, 0);
        if (max_step == 0) return h;

        const int limit = max_distance + max_step - 1;
        std::vector<uint32_t> dp(static_cast<size_t>(limit) + 1, INF32);
        dp[0] = 0;
        for (int s = 0; s <= limit; ++s) {
            if (dp[s] == INF32) continue;
            for (int p = 1; p <= max_step; ++p) {
                uint32_t c = step_cost[p];
                if (c == INF32 || s + p > limit) continue;
                uint32_t nd = dp[s] + c;
                if (nd < dp[s + p]) dp[s + p] = nd;
            }
        }

        for (int d = 1; d <= max_distance; ++d) {
            uint32_t best = INF32;
            int hi = std::min(limit, d + max_step - 1);
            for (int s = d; s <= hi; ++s) best = std::min(best, dp[s]);
            h[d] = (best == INF32) ? 0 : best;
        }
        return h;
    }

    void precompute_heuristic() {
        min_arc_by_net_.assign(nets_.size(), NO_U16);
        for (uint16_t in = 0; in < input_count_; ++in) {
            for (const MacroEdge& tr : transitions_[in]) {
                uint16_t& v = min_arc_by_net_[tr.net_id];
                if (tr.arc_delay < v) v = tr.arc_delay;
            }
        }

        std::vector<uint32_t> east(static_cast<size_t>(width_) + 1, INF32);
        std::vector<uint32_t> west(static_cast<size_t>(width_) + 1, INF32);
        std::vector<uint32_t> north(static_cast<size_t>(height_) + 1, INF32);
        std::vector<uint32_t> south(static_cast<size_t>(height_) + 1, INF32);

        for (int32_t cell = 0; cell < static_cast<int32_t>(cells_.size()); ++cell) {
            const Coord& a = cells_[cell];
            size_t base = static_cast<size_t>(cell) * nets_.size();
            for (uint16_t nid = 0; nid < nets_.size(); ++nid) {
                uint16_t arc = min_arc_by_net_[nid];
                if (arc == NO_U16) continue;
                int32_t dst = spatial_next_[base + nid];
                if (dst < 0) continue;
                const Coord& b = cells_[dst];
                uint32_t cost = static_cast<uint32_t>(arc) + spatial_extra_[base + nid];
                int dx = static_cast<int>(b.x) - static_cast<int>(a.x);
                int dy = static_cast<int>(b.y) - static_cast<int>(a.y);
                if (dx > 0) east[static_cast<size_t>(dx)] = std::min(east[static_cast<size_t>(dx)], cost);
                else if (dx < 0) west[static_cast<size_t>(-dx)] = std::min(west[static_cast<size_t>(-dx)], cost);
                else if (dy > 0) north[static_cast<size_t>(dy)] = std::min(north[static_cast<size_t>(dy)], cost);
                else if (dy < 0) south[static_cast<size_t>(-dy)] = std::min(south[static_cast<size_t>(-dy)], cost);
            }
        }

        h_east_  = build_direction_lb(east, width_ - 1);
        h_west_  = build_direction_lb(west, width_ - 1);
        h_north_ = build_direction_lb(north, height_ - 1);
        h_south_ = build_direction_lb(south, height_ - 1);

        std::cerr << "A* heuristic ready" << "\n";
    }

    void build_geom_moves() {
        geom_moves_.clear();
        for (uint16_t nid = 0; nid < nets_.size(); ++nid) {
            if (min_arc_by_net_[nid] == NO_U16) continue;
            const NetRule& n = nets_[nid];
            int found = -1;
            for (int i = 0; i < static_cast<int>(geom_moves_.size()); ++i) {
                if (geom_moves_[i].dx == n.dx && geom_moves_[i].dy == n.dy) { found = i; break; }
            }
            if (found < 0) {
                geom_moves_.push_back(GeomMove{n.dx, n.dy, nid, min_arc_by_net_[nid]});
            } else if (min_arc_by_net_[nid] < geom_moves_[found].min_arc) {
                // Spatial behavior depends only on dx/dy, so any representative Net is valid.
                geom_moves_[found].min_arc = min_arc_by_net_[nid];
            }
        }
    }

    std::vector<uint32_t> cell_dijkstra_forward(int32_t source) const {
        std::vector<uint32_t> d(cells_.size(), INF32);
        MinHeap hp;
        hp.reserve(cells_.size());
        d[static_cast<size_t>(source)] = 0;
        hp.push(HeapItem{0, 0, static_cast<uint32_t>(source)});
        while (!hp.empty()) {
            HeapItem q = hp.pop();
            int32_t cell = static_cast<int32_t>(q.state);
            if (d[static_cast<size_t>(cell)] != q.g) continue;
            size_t base = static_cast<size_t>(cell) * nets_.size();
            for (const GeomMove& gm : geom_moves_) {
                size_t si = base + gm.rep_net;
                int32_t nxt = spatial_next_[si];
                if (nxt < 0) continue;
                uint32_t nd = q.g + static_cast<uint32_t>(gm.min_arc) + spatial_extra_[si];
                if (nd < d[static_cast<size_t>(nxt)]) {
                    d[static_cast<size_t>(nxt)] = nd;
                    hp.push(HeapItem{nd, nd, static_cast<uint32_t>(nxt)});
                }
            }
        }
        return d;
    }

    std::vector<uint32_t> cell_dijkstra_reverse(int32_t source,
                                                const std::vector<uint32_t>& roff,
                                                const std::vector<uint32_t>& rsrc,
                                                const std::vector<uint16_t>& rcost) const {
        std::vector<uint32_t> d(cells_.size(), INF32);
        MinHeap hp;
        hp.reserve(cells_.size());
        d[static_cast<size_t>(source)] = 0;
        hp.push(HeapItem{0, 0, static_cast<uint32_t>(source)});
        while (!hp.empty()) {
            HeapItem q = hp.pop();
            uint32_t cell = q.state;
            if (d[cell] != q.g) continue;
            for (uint32_t ei = roff[cell]; ei < roff[cell + 1]; ++ei) {
                uint32_t nxt = rsrc[ei];
                uint32_t nd = q.g + rcost[ei];
                if (nd < d[nxt]) {
                    d[nxt] = nd;
                    hp.push(HeapItem{nd, nd, nxt});
                }
            }
        }
        return d;
    }

    int32_t nearest_cell_to(int tx, int ty) const {
        int32_t best = 0;
        int bestd = std::numeric_limits<int>::max();
        for (int32_t c = 0; c < static_cast<int32_t>(cells_.size()); ++c) {
            int d = std::abs(static_cast<int>(cells_[c].x) - tx) +
                    std::abs(static_cast<int>(cells_[c].y) - ty);
            if (d < bestd) { bestd = d; best = c; }
        }
        return best;
    }

    void precompute_landmarks() {
        build_geom_moves();
        landmark_cells_.clear();
        // 12 grid landmarks: 4 x positions * 3 y positions. They are snapped
        // to the nearest real SRB.
        const int xs[4] = {0, (width_ - 1) / 3, 2 * (width_ - 1) / 3, width_ - 1};
        const int ys[3] = {0, (height_ - 1) / 2, height_ - 1};
        for (int y : ys) for (int x : xs) {
            int32_t c = nearest_cell_to(x, y);
            if (std::find(landmark_cells_.begin(), landmark_cells_.end(), c) == landmark_cells_.end())
                landmark_cells_.push_back(c);
        }

        // Missing interior cells are Block/cut-out geometry.  Real cells next
        // to those holes are portal candidates.  Deterministic farthest-point
        // sampling spreads fourteen portal landmarks over all Block rims and
        // avoids serializing any contest-specific hand-picked coordinates.
        std::vector<int32_t> portal_candidates;
        portal_candidates.reserve(cells_.size() / 8);
        for (int32_t c = 0; c < static_cast<int32_t>(cells_.size()); ++c) {
            const int x = cells_[c].x;
            const int y = cells_[c].y;
            bool rim = false;
            const int nx[4] = {x - 1, x + 1, x, x};
            const int ny[4] = {y, y, y - 1, y + 1};
            for (int k = 0; k < 4; ++k) {
                if (nx[k] <= 0 || nx[k] >= width_ - 1 || ny[k] <= 0 || ny[k] >= height_ - 1) continue;
                if (cell_at(nx[k], ny[k]) < 0) { rim = true; break; }
            }
            if (rim) portal_candidates.push_back(c);
        }
        constexpr int PORTAL_LANDMARKS = 14;
        for (int pick = 0; pick < PORTAL_LANDMARKS && !portal_candidates.empty(); ++pick) {
            int32_t best_cell = -1;
            int best_score = -1;
            for (int32_t c : portal_candidates) {
                if (std::find(landmark_cells_.begin(), landmark_cells_.end(), c) != landmark_cells_.end()) continue;
                int nearest = std::numeric_limits<int>::max();
                for (int32_t chosen : landmark_cells_) {
                    const int d = std::abs(static_cast<int>(cells_[c].x) - cells_[chosen].x) +
                                  std::abs(static_cast<int>(cells_[c].y) - cells_[chosen].y);
                    nearest = std::min(nearest, d);
                }
                if (nearest > best_score || (nearest == best_score && c < best_cell)) {
                    best_score = nearest;
                    best_cell = c;
                }
            }
            if (best_cell < 0) break;
            landmark_cells_.push_back(best_cell);
        }

        const size_t C = cells_.size();
        landmark_from_.assign(landmark_cells_.size() * C, INF32);
        landmark_to_.assign(landmark_cells_.size() * C, INF32);

        // Build reverse CSR of the relaxed cell graph only for landmark preprocessing.
        std::vector<uint32_t> roff(C + 1, 0);
        uint64_t edge_count = 0;
        for (int32_t cell = 0; cell < static_cast<int32_t>(C); ++cell) {
            size_t base = static_cast<size_t>(cell) * nets_.size();
            for (const GeomMove& gm : geom_moves_) {
                int32_t dst = spatial_next_[base + gm.rep_net];
                if (dst >= 0) { ++roff[static_cast<size_t>(dst) + 1]; ++edge_count; }
            }
        }
        for (size_t i = 1; i < roff.size(); ++i) roff[i] += roff[i - 1];
        std::vector<uint32_t> cursor = roff;
        std::vector<uint32_t> rsrc(static_cast<size_t>(edge_count));
        std::vector<uint16_t> rcost(static_cast<size_t>(edge_count));
        for (int32_t cell = 0; cell < static_cast<int32_t>(C); ++cell) {
            size_t base = static_cast<size_t>(cell) * nets_.size();
            for (const GeomMove& gm : geom_moves_) {
                size_t si = base + gm.rep_net;
                int32_t dst = spatial_next_[si];
                if (dst < 0) continue;
                uint32_t cost = static_cast<uint32_t>(gm.min_arc) + spatial_extra_[si];
                if (cost >= NO_U16) throw std::runtime_error("relaxed cell edge cost exceeds uint16");
                uint32_t pos = cursor[static_cast<size_t>(dst)]++;
                rsrc[pos] = static_cast<uint32_t>(cell);
                rcost[pos] = static_cast<uint16_t>(cost);
            }
        }

        for (size_t li = 0; li < landmark_cells_.size(); ++li) {
            auto df = cell_dijkstra_forward(landmark_cells_[li]);
            auto dt = cell_dijkstra_reverse(landmark_cells_[li], roff, rsrc, rcost);
            std::copy(df.begin(), df.end(), landmark_from_.begin() + li * C);
            std::copy(dt.begin(), dt.end(), landmark_to_.begin() + li * C);
        }
        std::cerr << "ALT landmarks ready: " << landmark_cells_.size()
                  << " relaxed_moves=" << geom_moves_.size()
                  << " relaxed_edges=" << edge_count << "\n";
    }

    void select_query_landmarks(int32_t src, int32_t dst) {
        struct Cand { uint32_t score; int li; };
        std::vector<Cand> cs;
        cs.reserve(landmark_cells_.size());
        const size_t C = cells_.size();
        for (int li = 0; li < static_cast<int>(landmark_cells_.size()); ++li) {
            size_t base = static_cast<size_t>(li) * C;
            uint32_t dLs = landmark_from_[base + static_cast<size_t>(src)];
            uint32_t dLt = landmark_from_[base + static_cast<size_t>(dst)];
            uint32_t dsL = landmark_to_[base + static_cast<size_t>(src)];
            uint32_t dtL = landmark_to_[base + static_cast<size_t>(dst)];
            uint32_t score = 0;
            if (dLs < INF32 && dLt < INF32 && dLt > dLs) score = std::max(score, dLt - dLs);
            if (dsL < INF32 && dtL < INF32 && dsL > dtL) score = std::max(score, dsL - dtL);
            cs.push_back(Cand{score, li});
        }
        std::sort(cs.begin(), cs.end(), [](const Cand& a, const Cand& b) {
            if (a.score != b.score) return a.score > b.score;
            return a.li < b.li;
        });
        active_lm_count_ = std::min<int>(ACTIVE_LM, static_cast<int>(cs.size()));
        for (int k = 0; k < active_lm_count_; ++k) {
            int li = cs[k].li;
            active_lm_[k] = li;
            size_t base = static_cast<size_t>(li) * C;
            active_from_target_[k] = landmark_from_[base + static_cast<size_t>(dst)];
            active_to_target_[k] = landmark_to_[base + static_cast<size_t>(dst)];
        }
    }

    void build_relaxed_cell_reverse() {
        // Collapse the 160 lane-specific Nets into unique geometric moves. For each
        // geometry use the cheapest possible Arc among all lanes/Inputs. This makes
        // the cell graph a relaxation of the true graph, hence its exact distance is
        // an admissible A* lower bound.
        exact_geom_moves_.clear();
        for (uint16_t nid = 0; nid < nets_.size(); ++nid) {
            if (min_arc_by_net_[nid] == NO_U16) continue;
            const NetRule& n = nets_[nid];
            int found = -1;
            for (int i = 0; i < static_cast<int>(exact_geom_moves_.size()); ++i) {
                if (exact_geom_moves_[i].dx == n.dx && exact_geom_moves_[i].dy == n.dy) {
                    found = i; break;
                }
            }
            if (found < 0) {
                exact_geom_moves_.push_back(GeomMove{n.dx, n.dy, nid, min_arc_by_net_[nid]});
            } else if (min_arc_by_net_[nid] < exact_geom_moves_[found].min_arc) {
                exact_geom_moves_[found].min_arc = min_arc_by_net_[nid];
            }
        }

        const size_t C = cells_.size();
        cell_rev_off_.assign(C + 1, 0);
        uint64_t E = 0;
        for (int32_t cell = 0; cell < static_cast<int32_t>(C); ++cell) {
            size_t base = static_cast<size_t>(cell) * nets_.size();
            for (const GeomMove& gm : exact_geom_moves_) {
                int32_t dst = spatial_next_[base + gm.rep_net];
                if (dst >= 0) { ++cell_rev_off_[static_cast<size_t>(dst) + 1]; ++E; }
            }
        }
        for (size_t i = 1; i < cell_rev_off_.size(); ++i) cell_rev_off_[i] += cell_rev_off_[i - 1];
        std::vector<uint32_t> cur = cell_rev_off_;
        cell_rev_src_.resize(static_cast<size_t>(E));
        cell_rev_cost_.resize(static_cast<size_t>(E));
        for (int32_t cell = 0; cell < static_cast<int32_t>(C); ++cell) {
            size_t base = static_cast<size_t>(cell) * nets_.size();
            for (const GeomMove& gm : exact_geom_moves_) {
                size_t si = base + gm.rep_net;
                int32_t dst = spatial_next_[si];
                if (dst < 0) continue;
                uint32_t w = static_cast<uint32_t>(gm.min_arc) + spatial_extra_[si];
                if (w >= NO_U16) throw std::runtime_error("cell relaxed edge too large");
                uint32_t pos = cur[static_cast<size_t>(dst)]++;
                cell_rev_src_[pos] = static_cast<uint32_t>(cell);
                cell_rev_cost_[pos] = static_cast<uint16_t>(w);
            }
        }
        query_cell_h_.resize(C);
        std::cerr << "relaxed cell reverse graph: moves=" << exact_geom_moves_.size()
                  << " edges=" << E << "\n";
    }

    void compute_query_cell_lower_bound(int32_t target_cell) {
        std::fill(query_cell_h_.begin(), query_cell_h_.end(), INF32);
        MinHeap hp;
        hp.reserve(cells_.size());
        query_cell_h_[static_cast<size_t>(target_cell)] = 0;
        hp.push(HeapItem{0, 0, static_cast<uint32_t>(target_cell)});
        while (!hp.empty()) {
            HeapItem q = hp.pop();
            uint32_t cell = q.state;
            if (query_cell_h_[cell] != q.g) continue;
            for (uint32_t ei = cell_rev_off_[cell]; ei < cell_rev_off_[cell + 1]; ++ei) {
                uint32_t prev = cell_rev_src_[ei];
                uint32_t nd = q.g + cell_rev_cost_[ei];
                if (nd < query_cell_h_[prev]) {
                    query_cell_h_[prev] = nd;
                    hp.push(HeapItem{nd, nd, prev});
                }
            }
        }
    }

    void build_route_reverse() {
        route_rev_.assign(routing_input_count_, {});
        // Collapse duplicate source-route -> destination-route transitions to minimum Arc delay.
        std::vector<uint16_t> best(static_cast<size_t>(routing_input_count_) * routing_input_count_, NO_U16);
        for (uint16_t sr = 0; sr < routing_input_count_; ++sr) {
            uint16_t full_in = route_to_input_[sr];
            for (const MacroEdge& tr : transitions_[full_in]) {
                size_t k = static_cast<size_t>(sr) * routing_input_count_ + tr.next_input;
                best[k] = std::min(best[k], tr.arc_delay);
            }
        }
        for (uint16_t sr = 0; sr < routing_input_count_; ++sr) {
            for (uint16_t dr = 0; dr < routing_input_count_; ++dr) {
                uint16_t w = best[static_cast<size_t>(sr) * routing_input_count_ + dr];
                if (w != NO_U16) route_rev_[dr].push_back({sr, w});
            }
        }
        query_route_h_.resize(routing_input_count_);
    }

    void build_bidirectional_index() {
        reverse_transitions_.assign(routing_input_count_, {});
        for (uint16_t source_route = 0; source_route < routing_input_count_; ++source_route) {
            const uint16_t full_input = route_to_input_[source_route];
            for (const MacroEdge& edge : transitions_[full_input]) {
                if (edge.next_input >= routing_input_count_)
                    throw std::runtime_error("non-routing transition target in exact state graph");
                reverse_transitions_[edge.next_input].push_back(
                    ReverseMacroEdge{source_route, edge.net_id, edge.arc_delay});
            }
        }

        spatial_prev_.assign(spatial_next_.size(), -1);
        uint64_t collisions = 0;
        for (int32_t source = 0; source < static_cast<int32_t>(cells_.size()); ++source) {
            const size_t source_base = static_cast<size_t>(source) * nets_.size();
            for (uint16_t net = 0; net < nets_.size(); ++net) {
                const int32_t destination = spatial_next_[source_base + net];
                if (destination < 0) continue;
                const size_t reverse_index = static_cast<size_t>(destination) * nets_.size() + net;
                if (spatial_prev_[reverse_index] >= 0 && spatial_prev_[reverse_index] != source) {
                    ++collisions;
                } else {
                    spatial_prev_[reverse_index] = source;
                }
            }
        }
        if (collisions != 0) {
            throw std::runtime_error("spatial Net mapping is not injective; bidirectional index needs CSR");
        }
        std::cerr << "bidirectional reverse index ready: transitions=";
        size_t count = 0;
        for (const auto& edges : reverse_transitions_) count += edges.size();
        std::cerr << count << " spatial_entries=" << spatial_prev_.size() << "\n";
    }

    void compute_query_route_lower_bound(uint16_t dst_pid) {
        std::fill(query_route_h_.begin(), query_route_h_.end(), INF32);
        MinHeap hp;
        hp.reserve(routing_input_count_ * 4);
        if (port_is_input_[dst_pid]) {
            uint16_t full_in = static_cast<uint16_t>(port_to_input_[dst_pid]);
            int16_t tr = input_to_route_[full_in];
            if (tr < 0) return;
            query_route_h_[static_cast<uint16_t>(tr)] = 0;
            hp.push(HeapItem{0,0,static_cast<uint32_t>(tr)});
        } else {
            // Virtual terminal: any route with a direct Arc to dst Output is a seed.
            for (uint16_t r = 0; r < routing_input_count_; ++r) {
                uint16_t full_in = route_to_input_[r];
                uint16_t w = arc_delay_to_output(full_in, dst_pid);
                if (w == NO_U16) continue;
                if (w < query_route_h_[r]) {
                    query_route_h_[r] = w;
                    hp.push(HeapItem{w,w,static_cast<uint32_t>(r)});
                }
            }
        }
        while (!hp.empty()) {
            HeapItem q = hp.pop();
            uint16_t r = static_cast<uint16_t>(q.state);
            if (query_route_h_[r] != q.g) continue;
            for (const auto& e : route_rev_[r]) {
                uint16_t prev = e.first;
                uint32_t nd = q.g + e.second;
                if (nd < query_route_h_[prev]) {
                    query_route_h_[prev] = nd;
                    hp.push(HeapItem{nd,nd,static_cast<uint32_t>(prev)});
                }
            }
        }
    }

    void precompute_route_port_lower_bounds() {
        const size_t R = routing_input_count_;
        port_route_h_.assign(port_names_.size() * R, INF32);
        for (uint16_t pid = 0; pid < port_names_.size(); ++pid) {
            compute_query_route_lower_bound(pid);
            std::copy(query_route_h_.begin(), query_route_h_.end(),
                      port_route_h_.begin() + static_cast<size_t>(pid) * R);
        }
        std::cerr << "target-port lower bounds ready: ports=" << port_names_.size()
                  << " routes=" << routing_input_count_ << "\n";
    }

    inline uint32_t heuristic_cell(int32_t cell) const {
        const Coord& c = cells_[cell];
        const int dx = static_cast<int>(query_dst_x_) - c.x;
        const int dy = static_cast<int>(query_dst_y_) - c.y;
        uint32_t h = 0;
        if (dx > 0 && static_cast<size_t>(dx) < h_east_.size()) h = std::max(h, h_east_[dx]);
        else if (dx < 0 && static_cast<size_t>(-dx) < h_west_.size()) h = std::max(h, h_west_[-dx]);
        if (dy > 0 && static_cast<size_t>(dy) < h_north_.size()) h = std::max(h, h_north_[dy]);
        else if (dy < 0 && static_cast<size_t>(-dy) < h_south_.size()) h = std::max(h, h_south_[-dy]);

        const size_t C = cells_.size();
        for (int k = 0; k < active_lm_count_; ++k) {
            const size_t base = static_cast<size_t>(active_lm_[k]) * C;
            const uint32_t from_l = landmark_from_[base + static_cast<size_t>(cell)];
            const uint32_t to_l = landmark_to_[base + static_cast<size_t>(cell)];
            const uint32_t from_target = active_from_target_[k];
            const uint32_t to_target = active_to_target_[k];
            if (from_l < INF32 && from_target < INF32 && from_target > from_l)
                h = std::max(h, from_target - from_l);
            if (to_l < INF32 && to_target < INF32 && to_l > to_target)
                h = std::max(h, to_l - to_target);
        }
        return h;
    }

    inline uint32_t heuristic_state(uint32_t state) const {
        int32_t cell = static_cast<int32_t>(state / routing_input_count_);
        uint16_t route = static_cast<uint16_t>(state % routing_input_count_);
        uint32_t hc = heuristic_cell(cell);
        uint32_t hp = port_route_h_[static_cast<size_t>(query_dst_pid_) * routing_input_count_ + route];
        if (hp == INF32) hp = 0;
        return std::max(hc, hp);
    }

    void init_query_storage() {
        uint64_t states64 = static_cast<uint64_t>(cells_.size()) * routing_input_count_;
        if (states64 > std::numeric_limits<uint32_t>::max()) throw std::runtime_error("too many states for uint32 state id");
        size_t states = static_cast<size_t>(states64);
        dist_.assign(states, INF32);
        stamp_.assign(states, 0);
        heap_.reserve(1u << 20);
    }

    void ensure_parent_storage() {
        if (!parent_state_.empty()) return;
        const size_t states = static_cast<size_t>(cells_.size()) * routing_input_count_;
        parent_state_.assign(states, PARENT_ROOT_INPUT);
        parent_out_.assign(states, NO_U16);
    }

    void begin_query() {
        ++generation_;
        if (generation_ == 0) {
            std::fill(stamp_.begin(), stamp_.end(), 0);
            if (!reverse_stamp_.empty()) std::fill(reverse_stamp_.begin(), reverse_stamp_.end(), 0);
            generation_ = 1;
        }
    }

    void relax_root_input(uint32_t state, uint32_t d) {
        stamp_[state] = generation_;
        dist_[state] = d;
        if (query_need_path_) {
            parent_state_[state] = PARENT_ROOT_INPUT;
            parent_out_[state] = NO_U16;
        }
        heap_.push(HeapItem{d + heuristic_state(state), d, state});
    }

    void relax_root_output(uint32_t state, uint32_t d) {
        stamp_[state] = generation_;
        dist_[state] = d;
        if (query_need_path_) {
            parent_state_[state] = PARENT_ROOT_OUTPUT;
            parent_out_[state] = query_src_pid_;
        }
        heap_.push(HeapItem{d + heuristic_state(state), d, state});
    }

    void relax_root_nonrouting(uint32_t state, uint32_t d, uint16_t via_output) {
        if (stamp_[state] != generation_ || d < dist_[state]) {
            stamp_[state] = generation_;
            dist_[state] = d;
            if (query_need_path_) {
                parent_state_[state] = PARENT_ROOT_NONROUTING_INPUT;
                parent_out_[state] = via_output;
            }
            heap_.push(HeapItem{d + heuristic_state(state), d, state});
        }
    }

    void relax_from(uint32_t state, uint32_t nd, uint32_t prev_state, uint16_t via_output) {
        if (stamp_[state] != generation_ || nd < dist_[state]) {
            stamp_[state] = generation_;
            dist_[state] = nd;
            if (query_need_path_) {
                parent_state_[state] = prev_state;
                parent_out_[state] = via_output;
            }
            heap_.push(HeapItem{nd + heuristic_state(state), nd, state});
        }
        // Equal-distance alternative intentionally does not replace parent: deterministic first path.
    }

    std::vector<std::string> reconstruct(uint32_t terminal_state) const {
        std::vector<std::string> rev;
        uint32_t cur = terminal_state;

        while (true) {
            int32_t cell = static_cast<int32_t>(cur / routing_input_count_);
            uint16_t route = static_cast<uint16_t>(cur % routing_input_count_);
            uint16_t in = route_to_input_[route];
            uint16_t in_pid = input_pid_from_compressed(in);
            rev.push_back(format_node(cell, in_pid));

            uint32_t p = parent_state_[cur];
            if (p == PARENT_ROOT_INPUT) break;
            if (p == PARENT_ROOT_OUTPUT) {
                rev.push_back(format_node(query_src_cell_, query_src_pid_));
                break;
            }
            if (p == PARENT_ROOT_NONROUTING_INPUT) {
                rev.push_back(format_node(query_src_cell_, parent_out_[cur]));
                rev.push_back(format_node(query_src_cell_, query_src_pid_));
                break;
            }

            uint16_t out_pid = parent_out_[cur];
            int32_t prev_cell = static_cast<int32_t>(p / routing_input_count_);
            rev.push_back(format_node(prev_cell, out_pid));
            cur = p;
        }

        std::reverse(rev.begin(), rev.end());
        return rev;
    }

    uint16_t input_pid_from_compressed(uint16_t in) const {
        // 496 ports only; this is used only during path reconstruction, not in the search hot loop.
        for (uint16_t pid = 0; pid < port_names_.size(); ++pid) {
            if (port_is_input_[pid] && static_cast<uint16_t>(port_to_input_[pid]) == in) return pid;
        }
        throw std::runtime_error("compressed Input id has no global Port id");
    }
};
