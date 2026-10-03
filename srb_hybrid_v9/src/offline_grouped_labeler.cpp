#include "srb_core_v9.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Options {
    std::string graph;
    std::string input;
    std::string labels;
    std::string statistics;
    uint64_t max_group_expanded = 500000;
    uint64_t limit = 0;
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
        else if (argument == "--input") result.input = value(index, argc, argv, argument);
        else if (argument == "--labels") result.labels = value(index, argc, argv, argument);
        else if (argument == "--statistics") result.statistics = value(index, argc, argv, argument);
        else if (argument == "--max-group-expanded")
            result.max_group_expanded = std::stoull(value(index, argc, argv, argument));
        else if (argument == "--limit") result.limit = std::stoull(value(index, argc, argv, argument));
        else throw std::runtime_error("unknown argument: " + argument);
    }
    if (result.graph.empty() || result.input.empty() || result.labels.empty() ||
        result.statistics.empty()) {
        throw std::runtime_error(
            "usage: offline_grouped_labeler --graph graph.bin --input requests.csv "
            "--labels labels.csv --statistics statistics.csv "
            "[--max-group-expanded N] [--limit N]");
    }
    return result;
}

std::string trim_cr(std::string input) {
    if (!input.empty() && input.back() == '\r') input.pop_back();
    return input;
}

std::pair<std::string, std::string> parse_pair(const std::string& line) {
    const size_t comma = line.find(',');
    if (comma == std::string::npos || line.find(',', comma + 1) != std::string::npos)
        throw std::runtime_error("expected exactly two CSV fields");
    return {line.substr(0, comma), trim_cr(line.substr(comma + 1))};
}

struct Request {
    uint64_t row = 0;
    std::string from;
    std::string to;
};

} // namespace

int main(int argc, char** argv) {
    using Clock = std::chrono::steady_clock;
    try {
        const Options options = parse_options(argc, argv);
        SRBSolver solver;
        solver.load_binary(options.graph, false);

        std::ifstream input(options.input, std::ios::binary);
        std::ofstream labels(options.labels, std::ios::binary);
        std::ofstream statistics(options.statistics, std::ios::binary);
        if (!input || !labels || !statistics)
            throw std::runtime_error("cannot open an input/output file");
        std::string line;
        if (!std::getline(input, line)) throw std::runtime_error("empty request CSV");
        line = trim_cr(line);
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xef &&
            static_cast<unsigned char>(line[1]) == 0xbb &&
            static_cast<unsigned char>(line[2]) == 0xbf)
            line.erase(0, 3);
        if (line != "From,To" && line != "from,to")
            throw std::runtime_error("request header must be From,To");

        labels << "From,To,Delay,Reachable\n";
        statistics << "row,From,To,Reachable,Delay,GroupSize,GroupExpanded,GroupUs,Fallback,"
                      "FallbackExpanded,FallbackUs\n";
        uint64_t source_row = 0;
        std::vector<Request> requests;
        while (std::getline(input, line)) {
            ++source_row;
            if (options.limit && requests.size() >= options.limit) break;
            line = trim_cr(line);
            if (line.empty()) continue;
            const auto fields = parse_pair(line);
            requests.push_back(Request{source_row, fields.first, fields.second});
        }

        uint64_t groups = 0;
        uint64_t reachable = 0;
        uint64_t unreachable = 0;
        uint64_t fallbacks = 0;
        uint64_t group_expanded_total = 0;
        uint64_t fallback_expanded_total = 0;
        double group_us_total = 0.0;
        double fallback_us_total = 0.0;
        const auto all_started = Clock::now();
        for (size_t begin = 0; begin < requests.size();) {
            size_t end = begin + 1;
            while (end < requests.size() && requests[end].from == requests[begin].from) ++end;
            std::vector<std::string> destinations;
            destinations.reserve(end - begin);
            for (size_t index = begin; index < end; ++index)
                destinations.push_back(requests[index].to);

            const auto group_started = Clock::now();
            std::vector<QueryResult> results = solver.query_delays_same_source_spec(
                requests[begin].from, destinations, options.max_group_expanded);
            const double group_us =
                std::chrono::duration<double, std::micro>(Clock::now() - group_started).count();
            const uint64_t group_expanded = results.empty() ? 0 : results.front().expanded;
            ++groups;
            group_expanded_total += group_expanded;
            group_us_total += group_us;

            for (size_t offset = 0; offset < results.size(); ++offset) {
                QueryResult& result = results[offset];
                bool fallback = false;
                uint64_t fallback_expanded = 0;
                double fallback_us = 0.0;
                if (result.budget_exhausted) {
                    fallback = true;
                    ++fallbacks;
                    const auto fallback_started = Clock::now();
                    result = solver.query_delay_spec(requests[begin + offset].from,
                                                     requests[begin + offset].to);
                    fallback_us = std::chrono::duration<double, std::micro>(
                                      Clock::now() - fallback_started).count();
                    fallback_expanded = result.expanded;
                    fallback_us_total += fallback_us;
                    fallback_expanded_total += fallback_expanded;
                }
                const Request& request = requests[begin + offset];
                labels << request.from << ',' << request.to << ',';
                if (result.reachable) {
                    labels << result.delay << ",1\n";
                    ++reachable;
                } else {
                    labels << ",0\n";
                    ++unreachable;
                }
                statistics << request.row << ',' << request.from << ',' << request.to << ','
                           << (result.reachable ? 1 : 0) << ',';
                if (result.reachable) statistics << result.delay;
                statistics << ',' << (end - begin) << ',' << group_expanded << ','
                           << std::fixed << std::setprecision(3) << group_us << ','
                           << (fallback ? 1 : 0) << ',' << fallback_expanded << ','
                           << fallback_us << '\n';
            }
            begin = end;
            if ((begin % 10000) == 0 || begin == requests.size()) {
                labels.flush();
                statistics.flush();
                std::cerr << "labeled=" << begin << " groups=" << groups
                          << " fallback=" << fallbacks
                          << " mean_group_ms=" << (group_us_total / groups / 1000.0) << '\n';
            }
        }
        const double wall_seconds =
            std::chrono::duration<double>(Clock::now() - all_started).count();
        std::cerr << "complete rows=" << requests.size() << " groups=" << groups
                  << " reachable=" << reachable << " unreachable=" << unreachable
                  << " fallbacks=" << fallbacks << " group_expanded=" << group_expanded_total
                  << " fallback_expanded=" << fallback_expanded_total
                  << " group_seconds=" << (group_us_total / 1e6)
                  << " fallback_seconds=" << (fallback_us_total / 1e6)
                  << " wall_seconds=" << wall_seconds << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
