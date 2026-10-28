#include "srb_core_v9.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string graph;
    std::string states;
    std::string output;
    int direction = 0;
    int side_band = 6;
    int min_x = 58;
    int max_x = 107;
    int min_y = std::numeric_limits<int>::min();
    int max_y = std::numeric_limits<int>::max();
};

std::string value(int& index, int argc, char** argv, const std::string& name) {
    if (++index >= argc) throw std::runtime_error("missing value for " + name);
    return argv[index];
}

Options parse_options(int argc, char** argv) {
    Options result;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--graph") result.graph = value(index, argc, argv, argument);
        else if (argument == "--states") result.states = value(index, argc, argv, argument);
        else if (argument == "--output") result.output = value(index, argc, argv, argument);
        else if (argument == "--direction") {
            const std::string direction = value(index, argc, argv, argument);
            if (direction == "up") result.direction = 1;
            else if (direction == "down") result.direction = -1;
            else throw std::runtime_error("direction must be up or down");
        } else if (argument == "--side-band") {
            result.side_band = std::stoi(value(index, argc, argv, argument));
        } else if (argument == "--min-x") {
            result.min_x = std::stoi(value(index, argc, argv, argument));
        } else if (argument == "--max-x") {
            result.max_x = std::stoi(value(index, argc, argv, argument));
        } else if (argument == "--min-y") {
            result.min_y = std::stoi(value(index, argc, argv, argument));
        } else if (argument == "--max-y") {
            result.max_y = std::stoi(value(index, argc, argv, argument));
        } else {
            throw std::runtime_error("unknown argument: " + argument);
        }
    }
    if (result.graph.empty() || result.states.empty() || result.output.empty() ||
        result.direction == 0 || result.side_band < 1 || result.side_band > 12) {
        throw std::runtime_error(
            "usage: offline_periodic_portal_matrix --graph graph.bin "
            "--states state_equivalence.csv --direction up|down --side-band N "
            "--output matrix.bin [--min-x N --max-x N --min-y N --max-y N]");
    }
    if (result.min_y == std::numeric_limits<int>::min())
        result.min_y = result.direction > 0 ? 38 : 376;
    if (result.max_y == std::numeric_limits<int>::max())
        result.max_y = result.direction > 0 ? 173 : 511;
    return result;
}

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> fields;
    std::istringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ',')) fields.push_back(field);
    return fields;
}

struct DirectionState {
    std::string route;
    int span = 0;
};

std::vector<DirectionState> load_direction_states(const Options& options) {
    std::ifstream input(options.states, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open state equivalence CSV");
    std::string line;
    if (!std::getline(input, line)) throw std::runtime_error("empty state equivalence CSV");
    std::vector<DirectionState> result;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::vector<std::string> fields = split_csv(line);
        if (fields.size() < 5) throw std::runtime_error("bad state equivalence row");
        const int dx = std::stoi(fields[3]);
        const int dy = std::stoi(fields[4]);
        if (dx == 0 && ((dy > 0) == (options.direction > 0)))
            result.push_back(DirectionState{fields[1], std::abs(dy)});
    }
    if (result.size() != 40)
        throw std::runtime_error("expected 40 direction states, found " +
                                 std::to_string(result.size()));
    return result;
}

struct Event {
    int x = 0;
    int phase = 0;
    std::string route;
};

std::vector<Event> make_events(const Options& options,
                               const std::vector<DirectionState>& states) {
    std::vector<int> xs;
    for (int x = 76 - options.side_band; x < 76; ++x) xs.push_back(x);
    for (int x = 90; x < 90 + options.side_band; ++x) xs.push_back(x);
    std::vector<Event> result;
    for (int x : xs)
        for (const DirectionState& state : states)
            for (int phase = 1; phase <= state.span; ++phase)
                result.push_back(Event{x, phase, state.route});
    return result;
}

std::string endpoint(const Event& event, int boundary, int direction) {
    return "SRB_" + std::to_string(event.x) + "_" +
           std::to_string(boundary + direction * event.phase) + "/" + event.route;
}

template <class T>
void write_value(std::ostream& output, T value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

} // namespace

int main(int argc, char** argv) {
    using Clock = std::chrono::steady_clock;
    try {
        const Options options = parse_options(argc, argv);
        const std::vector<DirectionState> states = load_direction_states(options);
        const std::vector<Event> events = make_events(options, states);
        if (events.size() > std::numeric_limits<uint16_t>::max())
            throw std::runtime_error("Portal dimension exceeds uint16");
        const uint16_t dimension = static_cast<uint16_t>(events.size());
        const int source_boundary = options.direction > 0 ? 49 : 500;
        const int target_boundary = source_boundary + options.direction * 100;

        std::vector<std::string> targets;
        targets.reserve(events.size());
        for (const Event& event : events)
            targets.push_back(endpoint(event, target_boundary, options.direction));

        SRBSolver solver;
        solver.load_binary(options.graph, true);
        std::ofstream output(options.output, std::ios::binary);
        if (!output) throw std::runtime_error("cannot create Portal matrix");
        output.write("PPORT001", 8);
        write_value<uint16_t>(output, 1);
        write_value<uint16_t>(output, dimension);
        write_value<uint16_t>(output, static_cast<uint16_t>(options.side_band));
        write_value<uint16_t>(output, 1);
        write_value<int8_t>(output, static_cast<int8_t>(options.direction));
        const char padding[3]{};
        output.write(padding, sizeof(padding));

        uint64_t expanded = 0;
        uint64_t unreachable = 0;
        const auto started = Clock::now();
        for (std::size_t source = 0; source < events.size(); ++source) {
            const std::string from = endpoint(events[source], source_boundary, options.direction);
            std::vector<QueryResult> results = solver.query_delays_same_source_spec(
                from, targets, 10000000, false,
                options.min_x, options.max_x, options.min_y, options.max_y);
            if (results.size() != events.size())
                throw std::runtime_error("Portal result dimension mismatch");
            if (!results.empty()) expanded += results.front().expanded;
            for (const QueryResult& result : results) {
                if (result.budget_exhausted)
                    throw std::runtime_error("Portal search exhausted its group budget");
                uint16_t delay = 65535;
                if (result.reachable) {
                    if (result.delay >= 65535)
                        throw std::runtime_error("Portal delay exceeds uint16");
                    delay = static_cast<uint16_t>(result.delay);
                } else {
                    ++unreachable;
                }
                write_value<uint16_t>(output, delay);
            }
            if ((source + 1) % 40 == 0 || source + 1 == events.size()) {
                const double seconds = std::chrono::duration<double>(
                    Clock::now() - started).count();
                std::cerr << "sources=" << (source + 1) << '/' << events.size()
                          << " rows=" << (source + 1) * events.size()
                          << " seconds=" << seconds << '\n';
            }
        }
        const double seconds = std::chrono::duration<double>(
            Clock::now() - started).count();
        std::cerr << "complete dimension=" << dimension
                  << " entries=" << static_cast<uint64_t>(dimension) * dimension
                  << " unreachable=" << unreachable << " expanded=" << expanded
                  << " seconds=" << seconds << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
