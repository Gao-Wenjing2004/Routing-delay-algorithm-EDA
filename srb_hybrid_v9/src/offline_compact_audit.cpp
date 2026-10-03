#include "compact_exact.hpp"
#include "p2_estimator.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

struct Options {
    std::string input;
    std::string output;
    uint32_t max_expanded = 262144;
    uint64_t limit = 0;
};

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto value = [&]() -> std::string {
            if (++index >= argc) throw std::runtime_error("missing value for " + argument);
            return argv[index];
        };
        if (argument == "--input") options.input = value();
        else if (argument == "--output") options.output = value();
        else if (argument == "--max-expanded") options.max_expanded = std::stoul(value());
        else if (argument == "--limit") options.limit = std::stoull(value());
        else throw std::runtime_error("unknown argument: " + argument);
    }
    if (options.input.empty() || options.output.empty())
        throw std::runtime_error("--input and --output are required");
    return options;
}

void trim_cr(std::string& value) {
    if (!value.empty() && value.back() == '\r') value.pop_back();
}

std::pair<std::string, std::string> endpoints(const std::string& line) {
    const std::size_t first = line.find(',');
    if (first == std::string::npos) throw std::runtime_error("missing CSV comma");
    const std::size_t second = line.find(',', first + 1);
    std::string from = line.substr(0, first);
    std::string to = line.substr(first + 1, second == std::string::npos
        ? std::string::npos : second - first - 1);
    trim_cr(from);
    trim_cr(to);
    return {from, to};
}

}  // namespace

int main(int argc, char** argv) {
    using Clock = std::chrono::steady_clock;
    try {
        const Options options = parse_options(argc, argv);
        std::ifstream input(options.input, std::ios::binary);
        std::ofstream output(options.output, std::ios::binary);
        if (!input || !output) throw std::runtime_error("cannot open input/output");

        p2_runtime::Estimator estimator;
        auto solver = std::make_unique<v9_compact::Solver>();
        output << "row,From,To,reachable,completed,budget_exhausted,exact_delay,"
                  "v8_delay,expanded,elapsed_us,dx,dy,cheb,block_count,"
                  "horizontal_gaps,vertical_gaps\n";

        std::string line;
        if (!std::getline(input, line)) throw std::runtime_error("empty input");
        trim_cr(line);
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xef &&
            static_cast<unsigned char>(line[1]) == 0xbb &&
            static_cast<unsigned char>(line[2]) == 0xbf) line.erase(0, 3);
        if (line != "From,To" && line != "from,to")
            throw std::runtime_error("input header must be From,To");

        uint64_t row = 0;
        uint64_t completed = 0;
        uint64_t exhausted = 0;
        uint64_t unreachable = 0;
        uint64_t expanded = 0;
        double elapsed_us = 0.0;
        const auto wall_started = Clock::now();
        while (std::getline(input, line)) {
            trim_cr(line);
            if (line.empty()) continue;
            if (options.limit && row >= options.limit) break;
            const auto pair = endpoints(line);
            p2_runtime::DetailedPrediction prediction;
            if (!estimator.predict_spec_detailed(
                    {pair.first.data(), pair.first.size()},
                    {pair.second.data(), pair.second.size()}, prediction)) {
                throw std::runtime_error("bad endpoint at row " + std::to_string(row + 1));
            }
            const auto started = Clock::now();
            const v9_compact::Result result = solver->query(
                prediction.source, prediction.target, options.max_expanded);
            const double query_us =
                std::chrono::duration<double, std::micro>(Clock::now() - started).count();
            const bool done = result.reachable && !result.budget_exhausted;
            completed += done;
            exhausted += result.budget_exhausted;
            unreachable += !result.reachable && !result.budget_exhausted;
            expanded += result.expanded;
            elapsed_us += query_us;
            output << row << ',' << pair.first << ',' << pair.second << ','
                   << result.reachable << ',' << done << ',' << result.budget_exhausted << ',';
            if (done) output << result.delay;
            output << ',' << prediction.delay << ',' << result.expanded << ',' << query_us << ','
                   << prediction.environment.dx << ',' << prediction.environment.dy << ','
                   << prediction.environment.cheb << ',' << prediction.environment.block_count << ','
                   << prediction.environment.horizontal_gaps << ','
                   << prediction.environment.vertical_gaps << '\n';
            ++row;
            if (row % 1000 == 0) {
                output.flush();
                std::cerr << "audited=" << row << " completed=" << completed
                          << " exhausted=" << exhausted
                          << " mean_expanded=" << (expanded / row)
                          << " mean_us=" << (elapsed_us / row) << '\n';
            }
        }
        const double wall_seconds =
            std::chrono::duration<double>(Clock::now() - wall_started).count();
        std::cerr << "complete rows=" << row << " completed=" << completed
                  << " exhausted=" << exhausted << " unreachable=" << unreachable
                  << " expanded=" << expanded << " exact_seconds=" << (elapsed_us / 1e6)
                  << " wall_seconds=" << wall_seconds << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
