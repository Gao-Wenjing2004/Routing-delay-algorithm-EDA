#include "p2_estimator.hpp"
#ifdef V9_COMPACT_SUBMISSION
#include "compact_exact.hpp"
#else
#include "srb_core_v9.hpp"
#endif
#ifdef V9_STRUCTURED_P6
#include "p6_structured.hpp"
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string input;
    std::string output;
    std::string graph;
    std::string trace;
    std::string mode = "hybrid";
    std::string search = "astar";
    uint64_t max_expanded = 16;
    int distance_limit = -1;
    bool select_blocks = false;
    bool learned_selector = true;
    bool cell_heuristics = false;
    float uncertainty_min = std::numeric_limits<float>::infinity();
};

void trim(srb_fast::Slice& value) {
    while (value.size && (value.data[0] == ' ' || value.data[0] == '\t' || value.data[0] == '\r')) {
        ++value.data;
        --value.size;
    }
    while (value.size) {
        const char c = value.data[value.size - 1];
        if (c != ' ' && c != '\t' && c != '\r') break;
        --value.size;
    }
}

bool fields(srb_fast::Slice line, srb_fast::Slice& from, srb_fast::Slice& to) {
    const char* comma = static_cast<const char*>(std::memchr(line.data, ',', line.size));
    if (!comma) return false;
    from = {line.data, static_cast<std::size_t>(comma - line.data)};
    to = {comma + 1, line.size - from.size - 1};
    const char* last = static_cast<const char*>(std::memchr(to.data, ',', to.size));
    if (last) to.size = static_cast<std::size_t>(last - to.data);
    trim(from);
    trim(to);
    return from.size && to.size;
}

bool case_equal(srb_fast::Slice value, const char* expected) {
    if (value.size != std::strlen(expected)) return false;
    for (std::size_t i = 0; i < value.size; ++i) {
        char c = value.data[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        if (c != expected[i]) return false;
    }
    return true;
}

void append_number(std::string& output, uint64_t value) {
    char digits[32];
    char* end = digits + sizeof(digits);
    char* current = end;
    do {
        *--current = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    output.append(current, end);
}

void write_all(FILE* stream, const char* data, std::size_t size) {
    if (size && std::fwrite(data, 1, size, stream) != size)
        throw std::runtime_error("output write failed");
}

Options parse_options(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + name);
            return argv[++i];
        };
        if (a == "-in") o.input = value("-in");
        else if (a == "-out") o.output = value("-out");
        else if (a == "-graph") o.graph = value("-graph");
        else if (a == "--trace") o.trace = value("--trace");
        else if (a == "--mode") o.mode = value("--mode");
        else if (a == "--search") o.search = value("--search");
        else if (a == "--max-expanded") o.max_expanded = std::stoull(value("--max-expanded"));
        else if (a == "--distance-limit") o.distance_limit = std::stoi(value("--distance-limit"));
        else if (a == "--select-blocks") o.select_blocks = std::stoi(value("--select-blocks")) != 0;
        else if (a == "--no-learned-selector") o.learned_selector = false;
        else if (a == "--cell-heuristics") o.cell_heuristics = true;
        else if (a == "--uncertainty-min") o.uncertainty_min = std::stof(value("--uncertainty-min"));
        else if (a == "-threads") {
            if (std::stoi(value("-threads")) != 1) throw std::runtime_error("V9 is deterministic single-threaded code");
        } else {
            throw std::runtime_error("unknown argument: " + a);
        }
    }
    if (o.input.empty() || o.output.empty()) throw std::runtime_error("-in and -out are required");
    if (o.mode != "v8" && o.mode != "exact" && o.mode != "hybrid")
        throw std::runtime_error("--mode must be v8, exact, or hybrid");
    if (o.search != "astar" && o.search != "bidir")
        throw std::runtime_error("--search must be astar or bidir");
    if (std::filesystem::absolute(o.input).lexically_normal() ==
        std::filesystem::absolute(o.output).lexically_normal())
        throw std::runtime_error("input and output must differ");
    return o;
}

} // namespace

int main(int argc, char** argv) {
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    try {
        const Options options = parse_options(argc, argv);
        p2_runtime::Estimator v8;
#ifdef V9_COMPACT_SUBMISSION
        v9_compact::Solver exact;
#else
        std::unique_ptr<SRBSolver> exact;
        if (options.mode != "v8") {
            exact = std::make_unique<SRBSolver>();
            if (options.graph.empty()) exact->load_embedded_binary(argv[0], options.cell_heuristics);
            else exact->load_binary(options.graph, options.cell_heuristics);
            if (options.search == "bidir") exact->enable_bidirectional();
            exact->print_stats();
        }
#endif
#ifdef V9_STRUCTURED_P6
        p6_runtime::Solver structured;
#endif

        using File = std::unique_ptr<FILE, decltype(&std::fclose)>;
        File input(std::fopen(options.input.c_str(), "rb"), &std::fclose);
        File output(std::fopen(options.output.c_str(), "wb"), &std::fclose);
        if (!input || !output) throw std::runtime_error("cannot open input or output");
        std::setvbuf(input.get(), nullptr, _IOFBF, 1 << 20);
        std::setvbuf(output.get(), nullptr, _IOFBF, 1 << 20);
        std::ofstream trace;
        if (!options.trace.empty()) {
            trace.open(options.trace, std::ios::binary);
            if (!trace) throw std::runtime_error("cannot open trace output");
            trace << "row,selected,exact_completed,budget_exhausted,expanded,v8_delay,output_delay,"
                     "dx,dy,cheb,block_count,uncertainty,source_port,target_port,source_class,"
                     "target_class,source_route,target_route,exact_us\n";
        }

        constexpr std::size_t block_size = 4u << 20;
        std::vector<char> buffer(block_size + 4096);
        std::string rendered;
        rendered.reserve(block_size + block_size / 4);
        write_all(output.get(), "From,To,Delay\n", sizeof("From,To,Delay\n") - 1);
        std::size_t carry = 0;
        bool first = true;
        uint64_t processed = 0, invalid = 0, selected_count = 0;
        uint64_t exact_count = 0, fallback_count = 0, expanded_total = 0;
        double exact_us_total = 0.0;
#ifdef V9_STRUCTURED_P6
        uint64_t structured_selected = 0, structured_completed = 0;
        uint64_t structured_candidates = 0, structured_valid = 0, structured_transitions = 0;
        double structured_us_total = 0.0;
#endif
        while (true) {
            const std::size_t read = std::fread(buffer.data() + carry, 1, block_size, input.get());
            if (std::ferror(input.get())) throw std::runtime_error("input read failed");
            const bool eof = read < block_size;
            const std::size_t total = carry + read;
            if (!total) break;
            std::size_t position = 0;
            rendered.clear();
            while (position < total) {
                const char* newline = static_cast<const char*>(
                    std::memchr(buffer.data() + position, '\n', total - position));
                if (!newline && !eof) break;
                const std::size_t stop = newline ? static_cast<std::size_t>(newline - buffer.data()) : total;
                srb_fast::Slice line{buffer.data() + position, stop - position};
                position = newline ? stop + 1 : total;
                if (!line.size) continue;
                if (first) {
                    first = false;
                    if (line.size >= 3 && std::memcmp(line.data, "\xef\xbb\xbf", 3) == 0) {
                        line.data += 3;
                        line.size -= 3;
                    }
                    srb_fast::Slice hf, ht;
                    if (fields(line, hf, ht) && case_equal(hf, "from") && case_equal(ht, "to")) continue;
                }

                srb_fast::Slice from, to;
                p2_runtime::DetailedPrediction prediction;
                if (!fields(line, from, to) || !v8.predict_spec_detailed(from, to, prediction)) {
                    ++invalid;
                    continue;
                }

                bool selected = options.mode == "exact";
                if (options.mode == "hybrid") {
                    const int dx = prediction.environment.dx;
                    const int dy = prediction.environment.dy;
                    const bool target7_pattern = prediction.target_class == 7 &&
                        ((dx == 0 && dy == -1) || (dx == -1 && dy == -1) ||
                         (dx == 1 && dy == -1) || (dx == -2 && dy == 0));
                    const bool target12_pattern = prediction.target_class == 12 &&
                        ((dx == 1 && dy == 0) || (dx == 0 && dy == -2));
                    const bool learned = options.learned_selector &&
                        prediction.source_route == 160 && (target7_pattern || target12_pattern);
                    selected = learned || (options.distance_limit >= 0 &&
                                prediction.environment.cheb <= options.distance_limit) ||
                               (options.select_blocks && prediction.environment.block_count > 0) ||
                               prediction.uncertainty >= options.uncertainty_min;
                }
                uint32_t answer = prediction.delay;
#ifdef V9_COMPACT_SUBMISSION
                v9_compact::Result result;
#else
                QueryResult result;
#endif
                double exact_us = 0.0;
                if (selected) {
                    ++selected_count;
#ifndef V9_COMPACT_SUBMISSION
                    const std::string from_string(from.data, from.size);
                    const std::string to_string(to.data, to.size);
#endif
                    const auto q0 = Clock::now();
#ifdef V9_COMPACT_SUBMISSION
                    result = exact.query(prediction.source, prediction.target,
                                         static_cast<uint32_t>(options.max_expanded));
#else
                    result = options.search == "bidir"
                        ? exact->query_delay_spec_bounded_bidirectional(
                              from_string, to_string, options.max_expanded)
                        : exact->query_delay_spec_bounded(
                              from_string, to_string, options.max_expanded);
#endif
                    exact_us = std::chrono::duration<double, std::micro>(Clock::now() - q0).count();
                    exact_us_total += exact_us;
                    expanded_total += result.expanded;
                    if (result.reachable && !result.budget_exhausted) {
                        answer = result.delay;
                        ++exact_count;
                    } else {
                        ++fallback_count;
                    }
                }

#ifdef V9_STRUCTURED_P6
                if (options.mode == "hybrid" && structured.selected(prediction)) {
                    ++structured_selected;
                    const auto p6_started = Clock::now();
                    const p6_runtime::Result p6 = structured.query(prediction);
                    structured_us_total += std::chrono::duration<double, std::micro>(
                        Clock::now() - p6_started).count();
                    structured_candidates += p6.candidates;
                    structured_valid += p6.valid_candidates;
                    structured_transitions += p6.transitions;
                    if (p6.reachable) {
                        answer = p6.delay;
                        ++structured_completed;
                    }
                }
#endif

                rendered.append(from.data, from.size);
                rendered.push_back(',');
                rendered.append(to.data, to.size);
                rendered.push_back(',');
                append_number(rendered, answer);
                rendered.push_back('\n');
                if (trace) {
                    trace << processed << ',' << selected << ','
                          << (result.reachable && !result.budget_exhausted) << ','
                          << result.budget_exhausted << ',' << result.expanded << ','
                          << prediction.delay << ',' << answer << ','
                          << prediction.environment.dx << ',' << prediction.environment.dy << ','
                          << prediction.environment.cheb << ',' << prediction.environment.block_count << ','
                          << prediction.uncertainty << ',' << prediction.source.port << ','
                          << prediction.target.port << ',' << prediction.source_class << ','
                          << prediction.target_class << ',' << prediction.source_route << ','
                          << prediction.target_route << ',' << exact_us << '\n';
                }
                ++processed;
            }
            write_all(output.get(), rendered.data(), rendered.size());
            carry = total - position;
            if (carry > 4096) throw std::runtime_error("CSV row exceeds 4096 bytes");
            if (carry) std::memmove(buffer.data(), buffer.data() + position, carry);
            if (eof) break;
        }
        if (std::fflush(output.get()) != 0) throw std::runtime_error("output flush failed");
        const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
        std::cerr << "processed=" << processed << " invalid_rows=" << invalid
                  << " selected=" << selected_count << " exact=" << exact_count
                  << " fallback=" << fallback_count << " expanded=" << expanded_total
                  << " exact_us=" << exact_us_total
#ifdef V9_STRUCTURED_P6
                  << " p6_selected=" << structured_selected
                  << " p6_completed=" << structured_completed
                  << " p6_candidates=" << structured_candidates
                  << " p6_valid=" << structured_valid
                  << " p6_transitions=" << structured_transitions
                  << " p6_us=" << structured_us_total
#endif
                  << " elapsed=" << elapsed
#ifdef V9_STRUCTURED_P6
                  << " algorithm=V9-P6-structured+bounded-Astar+V8\n";
#else
                  << " algorithm=V9-bounded-Astar+V8\n";
#endif
        return invalid ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
