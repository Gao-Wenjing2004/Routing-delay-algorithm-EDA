#include "srb_core_v9.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Options {
    std::string graph, states, input, output, report;
    unsigned side_band = 6;
    uint64_t max_expanded = 100000000;
    uint64_t limit = 0;
};

std::string value(int& index, int argc, char** argv, const std::string& name) {
    if (++index >= argc) throw std::runtime_error("missing value for " + name);
    return argv[index];
}

Options options(int argc, char** argv) {
    Options result;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--graph") result.graph = value(index, argc, argv, argument);
        else if (argument == "--states") result.states = value(index, argc, argv, argument);
        else if (argument == "--input") result.input = value(index, argc, argv, argument);
        else if (argument == "--output") result.output = value(index, argc, argv, argument);
        else if (argument == "--report") result.report = value(index, argc, argv, argument);
        else if (argument == "--side-band") result.side_band = std::stoul(value(index, argc, argv, argument));
        else if (argument == "--max-expanded") result.max_expanded = std::stoull(value(index, argc, argv, argument));
        else if (argument == "--limit") result.limit = std::stoull(value(index, argc, argv, argument));
        else throw std::runtime_error("unknown argument: " + argument);
    }
    if (result.graph.empty() || result.states.empty() || result.input.empty() ||
        result.output.empty() || result.report.empty() || result.side_band < 1 || result.side_band > 12)
        throw std::runtime_error("offline_local_connector_teacher requires graph/states/input/output/report");
    return result;
}

std::pair<std::string, std::string> pair_fields(std::string line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto first = line.find(',');
    if (first == std::string::npos) throw std::runtime_error("bad CSV row");
    const auto second = line.find(',', first + 1);
    return {line.substr(0, first), line.substr(first + 1, second - first - 1)};
}

int endpoint_y(const std::string& specification) {
    const auto slash = specification.find('/');
    const auto separator = specification.rfind('_', slash);
    return std::stoi(specification.substr(separator + 1, slash - separator - 1));
}

struct State { std::string route; int span = 0; };
std::vector<State> states(const std::string& path, int direction) {
    std::ifstream input(path);
    std::string line;
    std::getline(input, line);
    std::vector<State> result;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> fields;
        std::stringstream stream(line);
        std::string field;
        while (std::getline(stream, field, ',')) fields.push_back(field);
        if (fields.size() < 5) throw std::runtime_error("bad state CSV");
        const int dx = std::stoi(fields[3]), dy = std::stoi(fields[4]);
        if (dx == 0 && dy != 0 && ((dy > 0) == (direction > 0)))
            result.push_back(State{fields[1], std::abs(dy)});
    }
    if (result.size() != 40) throw std::runtime_error("expected 40 vertical routing states");
    return result;
}

std::vector<std::string> events(const std::vector<State>& route_states, int direction,
                                int boundary, unsigned side_band) {
    std::vector<std::string> result;
    for (int side = 0; side < 2; ++side) {
        const int begin = side ? 90 : 76 - static_cast<int>(side_band);
        const int end = side ? 90 + static_cast<int>(side_band) : 76;
        for (int x = begin; x < end; ++x)
            for (const State& state : route_states)
                for (int phase = 1; phase <= state.span; ++phase)
                    result.push_back("SRB_" + std::to_string(x) + "_" +
                        std::to_string(boundary + direction * phase) + "/" + state.route);
    }
    return result;
}

template<class T> void write_value(std::ostream& output, T value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

uint16_t packed_delay(const QueryResult& result) {
    if (result.budget_exhausted) throw std::runtime_error("connector Dijkstra budget exhausted");
    if (!result.reachable) return 65535;
    if (result.delay >= 65535) throw std::runtime_error("connector delay exceeds uint16");
    return static_cast<uint16_t>(result.delay);
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const Options option = options(argc, argv);
        const auto up_states = states(option.states, 1);
        const auto down_states = states(option.states, -1);
        const uint16_t dimension = static_cast<uint16_t>(option.side_band * 2 * 160);
        SRBSolver solver;
        solver.load_binary(option.graph, true);
        solver.enable_bidirectional();
        std::ifstream input(option.input);
        std::ofstream output(option.output, std::ios::binary);
        std::ofstream report(option.report);
        if (!input || !output || !report) throw std::runtime_error("cannot open connector files");
        output.write("LCONV101", 8);
        write_value<uint16_t>(output, 1);
        write_value<uint16_t>(output, dimension);
        write_value<uint32_t>(output, 0);  // patched row count
        report << "From,To,Direction,Periods,SourceReachable,TargetReachable,ExpandedForward,ExpandedReverse\n";
        std::string line;
        std::getline(input, line);
        uint32_t rows = 0;
        while (std::getline(input, line) && (!option.limit || rows < option.limit)) {
            const auto endpoints = pair_fields(line);
            const int source_y = endpoint_y(endpoints.first), target_y = endpoint_y(endpoints.second);
            const int direction = target_y > source_y ? 1 : target_y < source_y ? -1 : 0;
            if (!direction) continue;
            int source_boundary, target_boundary;
            if (direction > 0) {
                source_boundary = ((source_y - 49 + 99) / 100) * 100 + 49;
                target_boundary = ((target_y - 50) / 100) * 100 + 49;
            } else {
                source_boundary = (source_y / 100) * 100;
                target_boundary = (target_y / 100 + 1) * 100;
            }
            const int periods = std::abs(target_boundary - source_boundary) / 100;
            if (periods < 1 || periods > 5) continue;
            const auto& route_states = direction > 0 ? up_states : down_states;
            const auto source_events = events(route_states, direction, source_boundary, option.side_band);
            const auto target_events = events(route_states, direction, target_boundary, option.side_band);
            const auto source = solver.query_delays_same_source_spec(
                endpoints.first, source_events, option.max_expanded, false);
            const auto target = solver.query_delays_same_target_spec(
                target_events, endpoints.second, option.max_expanded);
            if (source.size() != dimension || target.size() != dimension)
                throw std::runtime_error("connector dimension mismatch");
            write_value<int8_t>(output, static_cast<int8_t>(direction));
            write_value<uint8_t>(output, static_cast<uint8_t>(periods));
            write_value<uint16_t>(output, 0);
            uint32_t source_reachable = 0, target_reachable = 0;
            for (const auto& result : source) { const auto value = packed_delay(result); write_value(output, value); source_reachable += value != 65535; }
            for (const auto& result : target) { const auto value = packed_delay(result); write_value(output, value); target_reachable += value != 65535; }
            report << endpoints.first << ',' << endpoints.second << ',' << direction << ',' << periods << ','
                   << source_reachable << ',' << target_reachable << ','
                   << (source.empty() ? 0 : source.front().expanded) << ','
                   << (target.empty() ? 0 : target.front().expanded) << '\n';
            ++rows;
            std::cerr << "connector_rows=" << rows << " source_reachable=" << source_reachable
                      << " target_reachable=" << target_reachable << '\n';
        }
        output.seekp(12);
        write_value<uint32_t>(output, rows);
        std::cerr << "complete connector_rows=" << rows << " dimension=" << dimension << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
