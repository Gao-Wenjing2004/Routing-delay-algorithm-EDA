#include "srb_core_v9.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
struct Options {
    std::string graph, input, labels, statistics;
    uint64_t max_group_expanded = std::numeric_limits<uint64_t>::max();
    int min_x = std::numeric_limits<int>::min();
    int max_x = std::numeric_limits<int>::max();
    int min_y = std::numeric_limits<int>::min();
    int max_y = std::numeric_limits<int>::max();
};

std::string next(int& index, int argc, char** argv, const std::string& option) {
    if (++index >= argc) throw std::runtime_error("missing value for " + option);
    return argv[index];
}

Options options(int argc, char** argv) {
    Options value;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--graph") value.graph = next(index, argc, argv, option);
        else if (option == "--input") value.input = next(index, argc, argv, option);
        else if (option == "--labels") value.labels = next(index, argc, argv, option);
        else if (option == "--statistics") value.statistics = next(index, argc, argv, option);
        else if (option == "--max-group-expanded") value.max_group_expanded = std::stoull(next(index, argc, argv, option));
        else if (option == "--min-x") value.min_x = std::stoi(next(index, argc, argv, option));
        else if (option == "--max-x") value.max_x = std::stoi(next(index, argc, argv, option));
        else if (option == "--min-y") value.min_y = std::stoi(next(index, argc, argv, option));
        else if (option == "--max-y") value.max_y = std::stoi(next(index, argc, argv, option));
        else throw std::runtime_error("unknown option: " + option);
    }
    if (value.graph.empty() || value.input.empty() || value.labels.empty() || value.statistics.empty())
        throw std::runtime_error("usage: offline_reverse_grouped_labeler --graph graph.bin --input target_grouped.csv --labels labels.csv --statistics statistics.csv");
    return value;
}

std::pair<std::string, std::string> pair(const std::string& input) {
    const size_t comma = input.find(',');
    if (comma == std::string::npos || input.find(',', comma + 1) != std::string::npos)
        throw std::runtime_error("expected two CSV fields");
    std::string second = input.substr(comma + 1);
    if (!second.empty() && second.back() == '\r') second.pop_back();
    return {input.substr(0, comma), second};
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const Options option = options(argc, argv);
        SRBSolver solver;
        solver.load_binary(option.graph, false);
        solver.enable_bidirectional();
        std::ifstream input(option.input, std::ios::binary);
        std::ofstream labels(option.labels, std::ios::binary);
        std::ofstream statistics(option.statistics, std::ios::binary);
        if (!input || !labels || !statistics) throw std::runtime_error("cannot open input/output");
        std::string line;
        if (!std::getline(input, line)) throw std::runtime_error("empty input");
        labels << "From,To,Delay,Reachable\n";
        statistics << "Target,Rows,Expanded,ElapsedUs,BudgetExhausted\n";
        struct Request { std::string from, to; };
        std::vector<Request> requests;
        while (std::getline(input, line)) {
            if (line.empty()) continue;
            auto fields = pair(line);
            requests.push_back({std::move(fields.first), std::move(fields.second)});
        }
        uint64_t reachable = 0, exhausted = 0;
        for (size_t begin = 0; begin < requests.size();) {
            size_t end = begin + 1;
            while (end < requests.size() && requests[end].to == requests[begin].to) ++end;
            std::vector<std::string> sources;
            sources.reserve(end - begin);
            for (size_t index = begin; index < end; ++index) sources.push_back(requests[index].from);
            const auto started = std::chrono::steady_clock::now();
            const auto results = solver.query_delays_same_target_spec(
                sources, requests[begin].to, option.max_group_expanded,
                option.min_x, option.max_x, option.min_y, option.max_y);
            const double elapsed = std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - started).count();
            const uint64_t expanded = results.empty() ? 0 : results.front().expanded;
            uint64_t group_exhausted = 0;
            for (size_t offset = 0; offset < results.size(); ++offset) {
                const QueryResult& result = results[offset];
                labels << requests[begin + offset].from << ',' << requests[begin + offset].to << ',';
                if (result.reachable) {
                    labels << result.delay << ",1\n";
                    ++reachable;
                } else {
                    labels << "-1,0\n";
                }
                group_exhausted += result.budget_exhausted;
            }
            exhausted += group_exhausted;
            statistics << requests[begin].to << ',' << (end - begin) << ',' << expanded << ','
                       << elapsed << ',' << group_exhausted << '\n';
            begin = end;
        }
        std::cerr << "rows=" << requests.size() << " reachable=" << reachable
                  << " budget_exhausted=" << exhausted << '\n';
        return exhausted ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
