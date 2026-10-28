#include "srb_core_v9.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string graph;
    std::string gap;
    std::string input;
    std::string labels;
    std::string summaries;
    std::string paths;
    uint64_t offset = 0;
    uint64_t limit = 0;
    uint64_t max_group_expanded = 10000000;
    int min_x = std::numeric_limits<int>::min();
    int max_x = std::numeric_limits<int>::max();
    int min_y = std::numeric_limits<int>::min();
    int max_y = std::numeric_limits<int>::max();
    bool delay_only = false;
    bool grouped = false;
};

struct PathNode {
    int x = 0;
    int y = 0;
    std::string port;
};

struct GapLineSummary {
    bool vertical = false;
    int site = 0;
};

struct BlockSummary {
    int lower = 0;
    int upper = 0;
    int left = 0;
    int right = 0;
};

struct ArchitectureSummary {
    std::vector<GapLineSummary> gaps;
    std::vector<BlockSummary> blocks;
};

struct PathSummary {
    uint64_t path_nodes = 0;
    uint64_t arc_steps = 0;
    uint64_t net_steps = 0;
    uint64_t turns = 0;
    uint64_t horizontal_steps = 0;
    uint64_t vertical_steps = 0;
    uint64_t gap_crossings = 0;
    uint64_t block_segments = 0;
    uint64_t portal_events = 0;
    uint64_t path_hash = 1469598103934665603ull;
    std::string first_direction = "none";
    std::string last_direction = "none";
    std::string first_arc = "none";
    std::string last_arc = "none";
    std::string routing_state_sequence;
    std::string primitive_sequence;
    std::string turn_sequence;
    std::string move_signature;
    std::string portal_signature;
};

std::string option_value(int& index, int argc, char** argv, const std::string& name) {
    if (++index >= argc) throw std::runtime_error("missing value for " + name);
    return argv[index];
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--graph") options.graph = option_value(index, argc, argv, argument);
        else if (argument == "--gap") options.gap = option_value(index, argc, argv, argument);
        else if (argument == "--input") options.input = option_value(index, argc, argv, argument);
        else if (argument == "--labels") options.labels = option_value(index, argc, argv, argument);
        else if (argument == "--summaries") options.summaries = option_value(index, argc, argv, argument);
        else if (argument == "--paths") options.paths = option_value(index, argc, argv, argument);
        else if (argument == "--offset") options.offset = std::stoull(option_value(index, argc, argv, argument));
        else if (argument == "--limit") options.limit = std::stoull(option_value(index, argc, argv, argument));
        else if (argument == "--max-group-expanded")
            options.max_group_expanded = std::stoull(option_value(index, argc, argv, argument));
        else if (argument == "--min-x")
            options.min_x = std::stoi(option_value(index, argc, argv, argument));
        else if (argument == "--max-x")
            options.max_x = std::stoi(option_value(index, argc, argv, argument));
        else if (argument == "--min-y")
            options.min_y = std::stoi(option_value(index, argc, argv, argument));
        else if (argument == "--max-y")
            options.max_y = std::stoi(option_value(index, argc, argv, argument));
        else if (argument == "--delay-only") options.delay_only = true;
        else if (argument == "--grouped") options.grouped = true;
        else throw std::runtime_error("unknown argument: " + argument);
    }
    if (options.graph.empty() || options.gap.empty() || options.input.empty() ||
        options.labels.empty() || options.summaries.empty()) {
        throw std::runtime_error(
            "usage: offline_exact_labeler --graph graph.bin --gap SRB_Gap.json "
            "--input requests.csv --labels labels.csv --summaries summaries.csv "
            "[--paths paths.jsonl] [--offset N] [--limit N] [--delay-only] "
            "[--grouped] [--max-group-expanded N] "
            "[--min-x N --max-x N --min-y N --max-y N]");
    }
    return options;
}

std::string trim_cr(std::string value) {
    if (!value.empty() && value.back() == '\r') value.pop_back();
    return value;
}

std::pair<std::string, std::string> parse_csv_pair(const std::string& line) {
    const std::size_t comma = line.find(',');
    if (comma == std::string::npos || line.find(',', comma + 1) != std::string::npos)
        throw std::runtime_error("expected exactly two CSV fields: " + line);
    return {line.substr(0, comma), trim_cr(line.substr(comma + 1))};
}

PathNode parse_path_node(const std::string& specification) {
    const std::size_t slash = specification.find('/');
    if (slash == std::string::npos) throw std::runtime_error("bad path node: " + specification);
    const std::string instance = specification.substr(0, slash);
    if (instance.rfind("SRB_", 0) != 0) throw std::runtime_error("bad path instance: " + specification);
    const std::size_t separator = instance.find('_', 4);
    if (separator == std::string::npos) throw std::runtime_error("bad path coordinate: " + specification);
    return PathNode{
        std::stoi(instance.substr(4, separator - 4)),
        std::stoi(instance.substr(separator + 1)),
        specification.substr(slash + 1),
    };
}

ArchitectureSummary load_architecture_summary(const std::string& path) {
    const Json root = load_json(path);
    const Json& gap = root.at("Gap");
    ArchitectureSummary result;
    for (const Json& row : gap.at("Line").a) {
        result.gaps.push_back(GapLineSummary{sget(row, "direction") == "vertical", iget(row, "site")});
    }
    for (const Json& row : gap.at("Block").a) {
        result.blocks.push_back(BlockSummary{
            iget(row, "lower"), iget(row, "upper"), iget(row, "left"), iget(row, "right")});
    }
    return result;
}

bool segment_crosses_gap(const PathNode& first, const PathNode& second, const GapLineSummary& gap) {
    if (gap.vertical && first.y == second.y) {
        const int lower = std::min(first.x, second.x);
        const int upper = std::max(first.x, second.x);
        return lower <= gap.site && gap.site < upper;
    }
    if (!gap.vertical && first.x == second.x) {
        const int lower = std::min(first.y, second.y);
        const int upper = std::max(first.y, second.y);
        return lower <= gap.site && gap.site < upper;
    }
    return false;
}

bool segment_meets_block(const PathNode& first, const PathNode& second, const BlockSummary& block) {
    const int min_x = std::min(first.x, second.x);
    const int max_x = std::max(first.x, second.x);
    const int min_y = std::min(first.y, second.y);
    const int max_y = std::max(first.y, second.y);
    return min_x <= block.right && max_x >= block.left &&
           min_y <= block.upper && max_y >= block.lower;
}

bool beside_block(const PathNode& node, const BlockSummary& block) {
    const bool vertical_rim = (node.x == block.left - 1 || node.x == block.right + 1) &&
                              node.y >= block.lower && node.y <= block.upper;
    const bool horizontal_rim = (node.y == block.lower - 1 || node.y == block.upper + 1) &&
                                node.x >= block.left && node.x <= block.right;
    return vertical_rim || horizontal_rim;
}

void hash_text(uint64_t& hash, const std::string& value) {
    for (unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    hash ^= 0xffu;
    hash *= 1099511628211ull;
}

PathSummary summarize_path(const std::vector<std::string>& path,
                           const ArchitectureSummary& architecture) {
    PathSummary result;
    result.path_nodes = path.size();
    std::vector<PathNode> nodes;
    nodes.reserve(path.size());
    for (const std::string& value : path) {
        nodes.push_back(parse_path_node(value));
        hash_text(result.path_hash, value);
    }

    char previous_direction = 0;
    std::ostringstream moves;
    std::ostringstream portals;
    std::ostringstream route_states;
    std::ostringstream primitives;
    std::ostringstream turns;
    bool first_move = true;
    bool first_portal = true;
    bool first_route_state = true;
    bool first_primitive = true;
    bool first_turn = true;
    for (std::size_t index = 1; index < nodes.size(); ++index) {
        const PathNode& first = nodes[index - 1];
        const PathNode& second = nodes[index];
        const int dx = second.x - first.x;
        const int dy = second.y - first.y;
        if (dx == 0 && dy == 0) {
            ++result.arc_steps;
            const std::string arc = first.port + ">" + second.port;
            if (result.first_arc == "none") result.first_arc = arc;
            result.last_arc = arc;
            continue;
        }
        if (dx != 0 && dy != 0) throw std::runtime_error("non-axis-aligned Net in reconstructed path");
        ++result.net_steps;
        const char direction = dx != 0 ? 'H' : 'V';
        if (direction == 'H') ++result.horizontal_steps;
        else ++result.vertical_steps;
        if (!previous_direction) {
            result.first_direction = std::string(1, direction);
            turns << direction;
            first_turn = false;
        } else if (direction != previous_direction) {
            ++result.turns;
            if (!first_turn) turns << '>';
            turns << direction;
        }
        previous_direction = direction;
        result.last_direction = std::string(1, direction);

        if (!first_route_state) route_states << '>';
        first_route_state = false;
        route_states << second.port;
        if (!first_primitive) primitives << '|';
        first_primitive = false;
        primitives << direction << ':' << (direction == 'H' ? dx : dy)
                   << '@' << first.port << '>' << second.port;

        if (!first_move) moves << '|';
        first_move = false;
        moves << direction << ':' << dx << ':' << dy << ':' << first.port << '>' << second.port;

        bool portal = false;
        for (std::size_t gap_index = 0; gap_index < architecture.gaps.size(); ++gap_index) {
            if (!segment_crosses_gap(first, second, architecture.gaps[gap_index])) continue;
            ++result.gap_crossings;
            portal = true;
            if (!first_portal) portals << '|';
            first_portal = false;
            portals << 'G' << gap_index << '@' << second.x << ':' << second.y;
        }
        for (std::size_t block_index = 0; block_index < architecture.blocks.size(); ++block_index) {
            if (segment_meets_block(first, second, architecture.blocks[block_index])) {
                ++result.block_segments;
                portal = true;
                if (!first_portal) portals << '|';
                first_portal = false;
                portals << 'B' << block_index << '@' << second.x << ':' << second.y;
            } else if (beside_block(second, architecture.blocks[block_index])) {
                portal = true;
                if (!first_portal) portals << '|';
                first_portal = false;
                portals << 'R' << block_index << '@' << second.x << ':' << second.y;
            }
        }
        if (portal) ++result.portal_events;
    }
    result.move_signature = moves.str();
    result.portal_signature = portals.str();
    result.routing_state_sequence = route_states.str();
    result.primitive_sequence = primitives.str();
    result.turn_sequence = turns.str();
    return result;
}

std::string json_escape(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (char character : value) {
        if (character == '\\' || character == '"') result.push_back('\\');
        if (character == '\n') result += "\\n";
        else if (character == '\r') result += "\\r";
        else result.push_back(character);
    }
    return result;
}

void write_json_path(std::ostream& output, uint64_t row,
                     const std::string& from, const std::string& to,
                     const QueryResult& result) {
    output << "{\"row\":" << row << ",\"from\":\"" << json_escape(from)
           << "\",\"to\":\"" << json_escape(to) << "\",\"reachable\":"
           << (result.reachable ? "true" : "false") << ",\"delay\":" << result.delay
           << ",\"expanded\":" << result.expanded << ",\"path\":[";
    for (std::size_t index = 0; index < result.path.size(); ++index) {
        if (index) output << ',';
        output << '"' << json_escape(result.path[index]) << '"';
    }
    output << "]}\n";
}

} // namespace

int main(int argc, char** argv) {
    using Clock = std::chrono::steady_clock;
    try {
        const Options options = parse_options(argc, argv);
        const ArchitectureSummary architecture = load_architecture_summary(options.gap);
        SRBSolver solver;
        solver.load_binary(options.graph, true);
        solver.print_stats();

        std::ifstream input(options.input, std::ios::binary);
        std::ofstream labels(options.labels, std::ios::binary);
        std::ofstream summaries(options.summaries, std::ios::binary);
        std::ofstream paths;
        if (!options.paths.empty()) paths.open(options.paths, std::ios::binary);
        if (!input || !labels || !summaries || (!options.paths.empty() && !paths))
            throw std::runtime_error("cannot open one or more input/output files");

        std::string line;
        if (!std::getline(input, line)) throw std::runtime_error("empty request CSV");
        line = trim_cr(line);
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xef &&
            static_cast<unsigned char>(line[1]) == 0xbb && static_cast<unsigned char>(line[2]) == 0xbf)
            line.erase(0, 3);
        if (line != "From,To" && line != "from,to")
            throw std::runtime_error("request header must be From,To");

        labels << "From,To,Delay\n";
        summaries << "row,From,To,Reachable,Delay,Expanded,ElapsedUs,Chebyshev,PathNodes,ArcSteps,NetSteps,Turns,"
                     "HorizontalSteps,VerticalSteps,GapCrossings,BlockSegments,PortalEvents,"
                     "FirstDirection,LastDirection,FirstArc,LastArc,PathHash,RoutingStateSequence,"
                     "PrimitiveSequence,TurnSequence,MoveSignature,PortalSignature\n";

        uint64_t source_row = 0;
        uint64_t written = 0;
        uint64_t unreachable = 0;
        uint64_t expanded_total = 0;
        double elapsed_us_total = 0.0;
        const auto all_started = Clock::now();

        auto write_result = [&](uint64_t row, const std::pair<std::string, std::string>& fields,
                                const QueryResult& result, double elapsed_us) {
            const PathNode source = parse_path_node(fields.first);
            const PathNode target = parse_path_node(fields.second);
            const int chebyshev = std::max(std::abs(target.x - source.x), std::abs(target.y - source.y));
            if (result.budget_exhausted)
                throw std::runtime_error("exact query exhausted a budget at row " +
                                         std::to_string(row));
            if (!result.reachable) {
                ++unreachable;
                summaries << row << ',' << fields.first << ',' << fields.second
                          << ",0,," << result.expanded << ',' << std::fixed << std::setprecision(3)
                          << elapsed_us << ',' << chebyshev
                          << ",0,0,0,0,0,0,0,0,0,none,none,none,none,0,,,,,\n";
                if (paths) write_json_path(paths, row, fields.first, fields.second, result);
                ++written;
                expanded_total += result.expanded;
                elapsed_us_total += elapsed_us;
                return;
            }
            const PathSummary summary = options.delay_only
                ? PathSummary{}
                : summarize_path(result.path, architecture);

            labels << fields.first << ',' << fields.second << ',' << result.delay << '\n';
            summaries << row << ',' << fields.first << ',' << fields.second << ",1," << result.delay
                      << ',' << result.expanded << ',' << std::fixed << std::setprecision(3) << elapsed_us
                      << ',' << chebyshev << ',' << summary.path_nodes << ',' << summary.arc_steps
                      << ',' << summary.net_steps << ',' << summary.turns << ',' << summary.horizontal_steps
                      << ',' << summary.vertical_steps << ',' << summary.gap_crossings << ','
                      << summary.block_segments << ',' << summary.portal_events << ','
                      << summary.first_direction << ',' << summary.last_direction << ','
                      << summary.first_arc << ',' << summary.last_arc << ',' << summary.path_hash << ','
                      << summary.routing_state_sequence << ',' << summary.primitive_sequence << ','
                      << summary.turn_sequence << ',' << summary.move_signature << ','
                      << summary.portal_signature << '\n';
            if (paths) write_json_path(paths, row, fields.first, fields.second, result);

            ++written;
            expanded_total += result.expanded;
            elapsed_us_total += elapsed_us;
            if ((written % 1000) == 0) {
                labels.flush();
                summaries.flush();
                if (paths) paths.flush();
                std::cerr << "labeled=" << written << " source_row=" << row
                          << " mean_expanded=" << (expanded_total / written)
                          << " mean_ms=" << (elapsed_us_total / written / 1000.0) << '\n';
            }
        };

        if (!options.grouped) {
            while (std::getline(input, line)) {
                ++source_row;
                if (source_row <= options.offset) continue;
                if (options.limit && written >= options.limit) break;
                line = trim_cr(line);
                if (line.empty()) continue;
                const auto fields = parse_csv_pair(line);
                const auto started = Clock::now();
                QueryResult result = options.delay_only
                    ? solver.query_delay_spec(fields.first, fields.second)
                    : solver.query_spec(fields.first, fields.second);
                const double elapsed_us =
                    std::chrono::duration<double, std::micro>(Clock::now() - started).count();
                write_result(source_row, fields, result, elapsed_us);
            }
        } else {
            struct Request {
                uint64_t row = 0;
                std::pair<std::string, std::string> fields;
            };
            std::vector<Request> requests;
            while (std::getline(input, line)) {
                ++source_row;
                if (source_row <= options.offset) continue;
                if (options.limit && requests.size() >= options.limit) break;
                line = trim_cr(line);
                if (line.empty()) continue;
                requests.push_back(Request{source_row, parse_csv_pair(line)});
            }
            for (std::size_t begin = 0; begin < requests.size();) {
                std::size_t end = begin + 1;
                while (end < requests.size() &&
                       requests[end].fields.first == requests[begin].fields.first) ++end;
                std::vector<std::string> destinations;
                destinations.reserve(end - begin);
                for (std::size_t index = begin; index < end; ++index)
                    destinations.push_back(requests[index].fields.second);
                const auto started = Clock::now();
                std::vector<QueryResult> results = solver.query_delays_same_source_spec(
                    requests[begin].fields.first, destinations,
                    options.max_group_expanded, !options.delay_only,
                    options.min_x, options.max_x, options.min_y, options.max_y);
                const double group_us =
                    std::chrono::duration<double, std::micro>(Clock::now() - started).count();
                for (std::size_t index = begin; index < end; ++index) {
                    QueryResult result = std::move(results[index - begin]);
                    double elapsed_us = group_us / static_cast<double>(end - begin);
                    if (result.budget_exhausted) {
                        const auto fallback_started = Clock::now();
                        result = options.delay_only
                            ? solver.query_delay_spec(requests[index].fields.first,
                                                      requests[index].fields.second)
                            : solver.query_spec(requests[index].fields.first,
                                                requests[index].fields.second);
                        elapsed_us += std::chrono::duration<double, std::micro>(
                            Clock::now() - fallback_started).count();
                    }
                    write_result(requests[index].row, requests[index].fields, result, elapsed_us);
                }
                begin = end;
            }
        }
        const double wall_seconds =
            std::chrono::duration<double>(Clock::now() - all_started).count();
        std::cerr << "complete rows=" << written << " unreachable=" << unreachable
                  << " expanded=" << expanded_total
                  << " exact_seconds=" << (elapsed_us_total / 1e6)
                  << " wall_seconds=" << wall_seconds << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
