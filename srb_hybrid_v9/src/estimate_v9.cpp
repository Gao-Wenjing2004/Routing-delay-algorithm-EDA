#include "p2_estimator.hpp"
#ifdef V9_COMPACT_SUBMISSION
#include "compact_exact.hpp"
#else
#include "srb_core_v9.hpp"
#endif
#ifdef V9_STRUCTURED_P6
#include "p6_structured.hpp"
#endif
#ifdef V9_STRUCTURED_P8
#include "p8_macro.hpp"
#endif
#ifdef V9_PACKED_ATLAS
#include "p14_packed_atlas.hpp"
#endif
#ifdef P19_O1_FIXED
#include "p19_o1_data.hpp"
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

#if defined(P36_AVX2_DISPATCH) && (defined(__x86_64__) || defined(_M_X64))
#include <immintrin.h>
#if defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#elif defined(_MSC_VER)
#include <intrin.h>
#endif
#endif

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
#ifdef V9_DISABLE_LEARNED_SELECTOR
    bool learned_selector = false;
#else
    bool learned_selector = true;
#endif
    bool cell_heuristics = false;
    bool profile = false;
    float uncertainty_min = std::numeric_limits<float>::infinity();
};

struct ParsedRow {
    srb_fast::Slice raw{};
    srb_fast::Slice from{};
    srb_fast::Slice to{};
    srb_fast::Endpoint source{};
    srb_fast::Endpoint target{};
    uint32_t answer = 0;
    bool emit = true;
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
#ifndef P18_TWO_COLUMN_IO
    const char* last = static_cast<const char*>(std::memchr(to.data, ',', to.size));
    if (last) to.size = static_cast<std::size_t>(last - to.data);
#endif
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

char* append_number(char* output, uint32_t value) {
#ifdef P25_PAIR_NUMBER_FORMAT
    static constexpr char pairs[] =
        "00010203040506070809"
        "10111213141516171819"
        "20212223242526272829"
        "30313233343536373839"
        "40414243444546474849"
        "50515253545556575859"
        "60616263646566676869"
        "70717273747576777879"
        "80818283848586878889"
        "90919293949596979899";
    if (value < 10) {
        *output++ = static_cast<char>('0' + value);
        return output;
    }
    if (value < 100) {
        std::memcpy(output, pairs + value * 2, 2);
        return output + 2;
    }
    if (value < 10000) {
        const uint32_t high = value / 100;
        const uint32_t low = value - high * 100;
        if (high < 10) *output++ = static_cast<char>('0' + high);
        else {
            std::memcpy(output, pairs + high * 2, 2);
            output += 2;
        }
        std::memcpy(output, pairs + low * 2, 2);
        return output + 2;
    }
    if (value < 1000000) {
        const uint32_t high = value / 10000;
        const uint32_t remainder = value - high * 10000;
        const uint32_t middle = remainder / 100;
        const uint32_t low = remainder - middle * 100;
        if (high < 10) *output++ = static_cast<char>('0' + high);
        else {
            std::memcpy(output, pairs + high * 2, 2);
            output += 2;
        }
        std::memcpy(output, pairs + middle * 2, 2);
        std::memcpy(output + 2, pairs + low * 2, 2);
        return output + 4;
    }
    // Defensive fallback for values outside the architecture's practical
    // delay range.  It preserves full uint32_t formatting semantics.
    char chunks[10];
    char* current = chunks + sizeof(chunks);
    do {
        const uint32_t quotient = value / 100;
        const uint32_t remainder = value - quotient * 100;
        current -= 2;
        std::memcpy(current, pairs + remainder * 2, 2);
        value = quotient;
    } while (value >= 100);
    if (value >= 10) {
        current -= 2;
        std::memcpy(current, pairs + value * 2, 2);
    } else {
        *--current = static_cast<char>('0' + value);
    }
    const std::size_t size = static_cast<std::size_t>(chunks + sizeof(chunks) - current);
    std::memcpy(output, current, size);
    return output + size;
#else
    char digits[16];
    char* end = digits + sizeof(digits);
    char* current = end;
    do {
        *--current = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    const std::size_t size = static_cast<std::size_t>(end - current);
    std::memcpy(output, current, size);
    return output + size;
#endif
}

void write_all(FILE* stream, const char* data, std::size_t size) {
    if (size && std::fwrite(data, 1, size, stream) != size)
        throw std::runtime_error("output write failed");
}

#ifdef P36_AVX2_DISPATCH
using ByteFinder = const char* (*)(const char*, std::size_t);

const char* find_newline_scalar(const char* data, std::size_t size) {
    return static_cast<const char*>(std::memchr(data, '\n', size));
}

#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2")))
#endif
const char* find_newline_avx2(const char* data, std::size_t size) {
    const __m256i needle = _mm256_set1_epi8('\n');
    std::size_t offset = 0;
    while (offset + 32 <= size) {
        const __m256i bytes = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(data + offset));
        const uint32_t mask = static_cast<uint32_t>(
            _mm256_movemask_epi8(_mm256_cmpeq_epi8(bytes, needle)));
        if (mask) {
#if defined(_MSC_VER)
            unsigned long index = 0;
            _BitScanForward(&index, mask);
            return data + offset + index;
#else
            return data + offset + static_cast<unsigned>(__builtin_ctz(mask));
#endif
        }
        offset += 32;
    }
    const char* tail = static_cast<const char*>(
        std::memchr(data + offset, '\n', size - offset));
    return tail;
}
#endif

ByteFinder select_newline_finder() {
#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__)
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid_max(0, nullptr) >= 7 &&
        __get_cpuid(1, &eax, &ebx, &ecx, &edx) &&
        (ecx & bit_OSXSAVE) && (ecx & bit_AVX)) {
        unsigned xcr0_low = 0, xcr0_high = 0;
        __asm__ volatile("xgetbv" : "=a"(xcr0_low), "=d"(xcr0_high) : "c"(0));
        if ((xcr0_low & 0x6u) == 0x6u) {
            __cpuid_count(7, 0, eax, ebx, ecx, edx);
            if (ebx & bit_AVX2) return &find_newline_avx2;
        }
    }
#elif defined(_M_X64)
    int registers[4] = {};
    __cpuid(registers, 0);
    const int maximum = registers[0];
    __cpuidex(registers, 1, 0);
    if (maximum >= 7 && (registers[2] & (1 << 27)) &&
        (registers[2] & (1 << 28)) && (_xgetbv(0) & 0x6) == 0x6) {
        __cpuidex(registers, 7, 0);
        if (registers[1] & (1 << 5)) return &find_newline_avx2;
    }
#endif
    return &find_newline_scalar;
}
#endif

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
        else if (a == "--profile") o.profile = true;
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
#ifdef V9_STRUCTURED_P8
        p8_runtime::Solver macro_solver;
#endif
#ifdef V9_PACKED_ATLAS
        p14_runtime::PackedAtlas packed_atlas;
#endif
        const auto initialized = Clock::now();

        using File = std::unique_ptr<FILE, decltype(&std::fclose)>;
        File input(std::fopen(options.input.c_str(), "rb"), &std::fclose);
        File output(std::fopen(options.output.c_str(), "wb"), &std::fclose);
        if (!input || !output) throw std::runtime_error("cannot open input or output");
        std::setvbuf(input.get(), nullptr, _IOFBF, 8 << 20);
        std::setvbuf(output.get(), nullptr, _IOFBF, 8 << 20);
        std::ofstream trace;
        if (!options.trace.empty()) {
            trace.open(options.trace, std::ios::binary);
            if (!trace) throw std::runtime_error("cannot open trace output");
            trace << "row,selected,exact_completed,budget_exhausted,expanded,v8_delay,output_delay,"
                     "dx,dy,cheb,block_count,uncertainty,source_port,target_port,source_class,"
                     "target_class,source_route,target_route,exact_us\n";
        }

#ifdef P28_IO_BLOCK_MIB
        constexpr std::size_t block_size = static_cast<std::size_t>(P28_IO_BLOCK_MIB) << 20;
#else
        constexpr std::size_t block_size = 16u << 20;
#endif
        std::vector<char> buffer(block_size + 4096);
#ifdef P27_FIXED_RENDER_BUFFER
        std::vector<char> rendered((block_size + 4096) * 2 + 64);
#else
        std::vector<char> rendered;
        rendered.reserve(block_size + block_size / 2);
#endif
        std::vector<ParsedRow> rows;
#ifndef P26_FUSED_DEFAULT_PIPELINE
        rows.reserve(block_size / 32);
#endif
#ifdef P35_FAST_SOA
        // The official two-column path only needs the original line, two
        // already-integer endpoints and the answer.  Keep those streams
        // separate so prediction does not pull the three Slice objects and
        // formatting metadata in ParsedRow through the cache.
        std::vector<srb_fast::Slice> fast_raw;
        std::vector<srb_fast::Endpoint> fast_sources;
        std::vector<srb_fast::Endpoint> fast_targets;
        std::vector<uint32_t> fast_answers;
        fast_raw.reserve(block_size / 32);
        fast_sources.reserve(block_size / 32);
        fast_targets.reserve(block_size / 32);
        fast_answers.reserve(block_size / 32);
#ifdef P36_AVX2_DISPATCH
        const ByteFinder find_line_break = select_newline_finder();
#endif
#endif
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
#ifdef V9_STRUCTURED_P8
        uint64_t macro_selected = 0, macro_completed = 0;
        uint64_t macro_candidates = 0, macro_valid = 0, macro_transitions = 0;
        double macro_us_total = 0.0;
#endif
#ifdef V9_PACKED_ATLAS
        uint64_t atlas_selected = 0, atlas_completed = 0;
#endif
        double read_seconds = 0.0, parse_seconds = 0.0, predict_seconds = 0.0;
        double format_seconds = 0.0, write_seconds = 0.0;
#ifdef P26_FUSED_DEFAULT_PIPELINE
        double fused_seconds = 0.0;
#endif
#ifdef P24_DEFAULT_SUBMISSION_FAST_PATH
        const bool default_submission_path =
            options.mode == "hybrid" && options.distance_limit < 0 &&
            !options.select_blocks && !options.learned_selector &&
            options.uncertainty_min == std::numeric_limits<float>::infinity() &&
            options.trace.empty();
#ifdef P26_FUSED_DEFAULT_PIPELINE
        if (!default_submission_path) rows.reserve(block_size / 32);
#endif
#endif
        while (true) {
            const auto read_started = Clock::now();
            const std::size_t read = std::fread(buffer.data() + carry, 1, block_size, input.get());
            read_seconds += std::chrono::duration<double>(Clock::now() - read_started).count();
            if (std::ferror(input.get())) throw std::runtime_error("input read failed");
            const bool eof = read < block_size;
            const std::size_t total = carry + read;
            if (!total) break;

#ifdef P26_FUSED_DEFAULT_PIPELINE
            if (default_submission_path) {
                const auto fused_started = Clock::now();
                std::size_t position = 0;
#ifndef P27_FIXED_RENDER_BUFFER
                rendered.resize(total * 2 + 64);
#endif
                char* rendered_begin = rendered.data();
                char* rendered_current = rendered_begin;
                while (position < total) {
                    const char* newline = static_cast<const char*>(
                        std::memchr(buffer.data() + position, '\n', total - position));
                    if (!newline && !eof) break;
                    const std::size_t stop = newline
                        ? static_cast<std::size_t>(newline - buffer.data()) : total;
                    srb_fast::Slice line{buffer.data() + position, stop - position};
                    position = newline ? stop + 1 : total;
                    if (!line.size) continue;
                    if (first) {
                        first = false;
                        if (line.size >= 3 && std::memcmp(line.data, "\xef\xbb\xbf", 3) == 0) {
                            line.data += 3;
                            line.size -= 3;
                        }
                        srb_fast::Slice header_from, header_to;
                        if (fields(line, header_from, header_to) &&
                            case_equal(header_from, "from") && case_equal(header_to, "to")) {
                            continue;
                        }
                    }

                    srb_fast::Slice from, to;
                    srb_fast::Endpoint source, target;
                    if (!fields(line, from, to) ||
                        !v8.parse_spec(from, to, source, target)) {
                        ++invalid;
                        continue;
                    }
                    srb_v4::Prediction prediction;
                    if (!v8.predict_endpoints_base(source, target, prediction)) {
                        ++invalid;
                        continue;
                    }
                    uint32_t answer = prediction.delay;
#ifdef V9_PACKED_ATLAS
#ifdef P32_ATLAS_QUERY_RADIUS
                    static constexpr int kOnlineAtlasRadius = P32_ATLAS_QUERY_RADIUS;
#else
                    static constexpr int kOnlineAtlasRadius = p14_runtime::PackedAtlas::kQueryRadius;
#endif
                    if (prediction.chebyshev <= kOnlineAtlasRadius &&
                        prediction.intersected_blocks == 0) {
                        ++atlas_selected;
                        uint32_t local = 0;
                        if (packed_atlas.query(source, target, local)) {
                            answer = local;
                            ++atlas_completed;
                        }
                    }
#endif
#ifdef P19_O1_FIXED
                    answer = p19_o1::apply(answer, source, target, prediction);
#endif
                    while (line.size && line.data[line.size - 1] == '\r') --line.size;
                    std::memcpy(rendered_current, line.data, line.size);
                    rendered_current += line.size;
                    *rendered_current++ = ',';
                    rendered_current = append_number(rendered_current, answer);
                    *rendered_current++ = '\n';
                    ++processed;
                }
                const std::size_t rendered_bytes =
                    static_cast<std::size_t>(rendered_current - rendered_begin);
#ifndef P27_FIXED_RENDER_BUFFER
                rendered.resize(rendered_bytes);
#endif
                fused_seconds += std::chrono::duration<double>(
                    Clock::now() - fused_started).count();

                const auto write_started = Clock::now();
                write_all(output.get(), rendered.data(), rendered_bytes);
                write_seconds += std::chrono::duration<double>(
                    Clock::now() - write_started).count();
                carry = total - position;
                if (carry > 4096) throw std::runtime_error("CSV row exceeds 4096 bytes");
                if (carry) std::memmove(buffer.data(), buffer.data() + position, carry);
                if (eof) break;
                continue;
            }
#endif

#ifdef P35_FAST_SOA
            if (default_submission_path) {
                const auto parse_started = Clock::now();
                std::size_t position = 0;
                fast_raw.clear();
                fast_sources.clear();
                fast_targets.clear();
                while (position < total) {
#ifdef P36_AVX2_DISPATCH
                    const char* newline = find_line_break(
                        buffer.data() + position, total - position);
#else
                    const char* newline = static_cast<const char*>(
                        std::memchr(buffer.data() + position, '\n', total - position));
#endif
                    if (!newline && !eof) break;
                    const std::size_t stop = newline
                        ? static_cast<std::size_t>(newline - buffer.data()) : total;
                    srb_fast::Slice line{buffer.data() + position, stop - position};
                    position = newline ? stop + 1 : total;
                    if (!line.size) continue;
                    if (first) {
                        first = false;
                        if (line.size >= 3 && std::memcmp(line.data, "\xef\xbb\xbf", 3) == 0) {
                            line.data += 3;
                            line.size -= 3;
                        }
                        srb_fast::Slice header_from, header_to;
                        if (fields(line, header_from, header_to) &&
                            case_equal(header_from, "from") && case_equal(header_to, "to")) {
                            continue;
                        }
                    }

                    srb_fast::Slice from, to;
                    srb_fast::Endpoint source, target;
                    if (!fields(line, from, to) ||
                        !v8.parse_spec(from, to, source, target)) {
                        ++invalid;
                        continue;
                    }
                    while (line.size && line.data[line.size - 1] == '\r') --line.size;
                    fast_raw.push_back(line);
                    fast_sources.push_back(source);
                    fast_targets.push_back(target);
                }
                parse_seconds += std::chrono::duration<double>(
                    Clock::now() - parse_started).count();

                const auto predict_started = Clock::now();
                fast_answers.resize(fast_sources.size());
                for (std::size_t index = 0; index < fast_sources.size(); ++index) {
                    const srb_fast::Endpoint& source = fast_sources[index];
                    const srb_fast::Endpoint& target = fast_targets[index];
                    srb_v4::Prediction prediction;
                    if (!v8.predict_endpoints_base(source, target, prediction)) {
                        // The base estimator is total over every successfully
                        // parsed endpoint.  Retain a defensive sentinel so a
                        // future implementation cannot silently emit a row.
                        fast_answers[index] = UINT32_MAX;
                        ++invalid;
                        continue;
                    }
                    uint32_t answer = prediction.delay;
#ifdef V9_PACKED_ATLAS
#ifdef P32_ATLAS_QUERY_RADIUS
                    static constexpr int kOnlineAtlasRadius = P32_ATLAS_QUERY_RADIUS;
#else
                    static constexpr int kOnlineAtlasRadius = p14_runtime::PackedAtlas::kQueryRadius;
#endif
                    if (prediction.chebyshev <= kOnlineAtlasRadius &&
                        prediction.intersected_blocks == 0) {
                        ++atlas_selected;
                        uint32_t local = 0;
                        if (packed_atlas.query(source, target, local)) {
                            answer = local;
                            ++atlas_completed;
                        }
                    }
#endif
#ifdef P19_O1_FIXED
                    answer = p19_o1::apply(answer, source, target, prediction);
#endif
                    fast_answers[index] = answer;
                    ++processed;
                }
                predict_seconds += std::chrono::duration<double>(
                    Clock::now() - predict_started).count();

                const auto format_started = Clock::now();
                rendered.resize(total + fast_raw.size() * 16 + 64);
                char* rendered_begin = rendered.data();
                char* rendered_current = rendered_begin;
                for (std::size_t index = 0; index < fast_raw.size(); ++index) {
                    if (fast_answers[index] == UINT32_MAX) continue;
                    const srb_fast::Slice line = fast_raw[index];
                    std::memcpy(rendered_current, line.data, line.size);
                    rendered_current += line.size;
                    *rendered_current++ = ',';
                    rendered_current = append_number(rendered_current, fast_answers[index]);
                    *rendered_current++ = '\n';
                }
                const std::size_t rendered_bytes =
                    static_cast<std::size_t>(rendered_current - rendered_begin);
                rendered.resize(rendered_bytes);
                format_seconds += std::chrono::duration<double>(
                    Clock::now() - format_started).count();

                const auto write_started = Clock::now();
                write_all(output.get(), rendered.data(), rendered_bytes);
                write_seconds += std::chrono::duration<double>(
                    Clock::now() - write_started).count();
                carry = total - position;
                if (carry > 4096) throw std::runtime_error("CSV row exceeds 4096 bytes");
                if (carry) std::memmove(buffer.data(), buffer.data() + position, carry);
                if (eof) break;
                continue;
            }
#endif

            const auto parse_started = Clock::now();
            std::size_t position = 0;
            rows.clear();
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

                ParsedRow row;
                if (!fields(line, row.from, row.to) ||
                    !v8.parse_spec(row.from, row.to, row.source, row.target)) {
                    ++invalid;
                    continue;
                }
#ifdef P18_TWO_COLUMN_IO
                row.raw = line;
                while (row.raw.size && row.raw.data[row.raw.size - 1] == '\r') --row.raw.size;
#endif
                rows.push_back(row);
            }
            parse_seconds += std::chrono::duration<double>(Clock::now() - parse_started).count();

            const auto predict_started = Clock::now();
            for (ParsedRow& row : rows) {
#ifdef P24_DEFAULT_SUBMISSION_FAST_PATH
                if (default_submission_path) {
                    srb_v4::Prediction prediction;
                    if (!v8.predict_endpoints_base(row.source, row.target, prediction)) {
                        row.emit = false;
                        ++invalid;
                        continue;
                    }
                    uint32_t answer = prediction.delay;
#ifdef V9_PACKED_ATLAS
#ifdef P32_ATLAS_QUERY_RADIUS
                    static constexpr int kOnlineAtlasRadius = P32_ATLAS_QUERY_RADIUS;
#else
                    static constexpr int kOnlineAtlasRadius = p14_runtime::PackedAtlas::kQueryRadius;
#endif
                    if (prediction.chebyshev <= kOnlineAtlasRadius &&
                        prediction.intersected_blocks == 0) {
                        ++atlas_selected;
                        uint32_t local = 0;
                        if (packed_atlas.query(row.source, row.target, local)) {
                            answer = local;
                            ++atlas_completed;
                        }
                    }
#endif
#ifdef P19_O1_FIXED
                    answer = p19_o1::apply(answer, row.source, row.target, prediction);
#endif
                    row.answer = answer;
                    ++processed;
                    continue;
                }
#endif
                p2_runtime::DetailedPrediction prediction;
                if (!v8.predict_endpoints_detailed(row.source, row.target, prediction)) {
                    row.emit = false;
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
#ifdef V9_PACKED_ATLAS
#ifdef P32_ATLAS_QUERY_RADIUS
                static constexpr int kOnlineAtlasRadius = P32_ATLAS_QUERY_RADIUS;
#else
                static constexpr int kOnlineAtlasRadius = p14_runtime::PackedAtlas::kQueryRadius;
#endif
                if (prediction.environment.cheb <= kOnlineAtlasRadius &&
                    prediction.environment.block_count == 0) {
                    ++atlas_selected;
                    uint32_t local = 0;
                    if (packed_atlas.query(prediction.source, prediction.target, local)) {
                        answer = local;
                        ++atlas_completed;
                    }
                }
#endif
#ifdef V9_COMPACT_SUBMISSION
                v9_compact::Result result;
#else
                QueryResult result;
#endif
                double exact_us = 0.0;
                bool exact_solved = false;
                if (selected) {
                    ++selected_count;
#ifndef V9_COMPACT_SUBMISSION
                    const std::string from_string(row.from.data, row.from.size);
                    const std::string to_string(row.to.data, row.to.size);
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
                        exact_solved = true;
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

#ifdef V9_STRUCTURED_P8
                if (options.mode == "hybrid" && !exact_solved && macro_solver.selected(prediction)) {
                    ++macro_selected;
                    const auto p8_started = Clock::now();
                    const p8_runtime::Result p8 = macro_solver.query(prediction);
                    macro_us_total += std::chrono::duration<double, std::micro>(
                        Clock::now() - p8_started).count();
                    macro_candidates += p8.candidates;
                    macro_valid += p8.valid_candidates;
                    macro_transitions += p8.transitions;
                    if (p8.reachable) {
                        answer = p8.delay;
                        ++macro_completed;
                    }
                }
#endif
                row.answer = answer;
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
            predict_seconds += std::chrono::duration<double>(Clock::now() - predict_started).count();

            const auto format_started = Clock::now();
#ifndef P27_FIXED_RENDER_BUFFER
            rendered.resize(total + rows.size() * 16 + 64);
#endif
            char* rendered_begin = rendered.data();
            char* rendered_current = rendered_begin;
            for (const ParsedRow& row : rows) {
                if (!row.emit) continue;
#ifdef P18_TWO_COLUMN_IO
                std::memcpy(rendered_current, row.raw.data, row.raw.size);
                rendered_current += row.raw.size;
#else
                std::memcpy(rendered_current, row.from.data, row.from.size);
                rendered_current += row.from.size;
                *rendered_current++ = ',';
                std::memcpy(rendered_current, row.to.data, row.to.size);
                rendered_current += row.to.size;
#endif
                *rendered_current++ = ',';
                rendered_current = append_number(rendered_current, row.answer);
                *rendered_current++ = '\n';
            }
            const std::size_t rendered_bytes =
                static_cast<std::size_t>(rendered_current - rendered_begin);
#ifndef P27_FIXED_RENDER_BUFFER
            rendered.resize(rendered_bytes);
#endif
            format_seconds += std::chrono::duration<double>(Clock::now() - format_started).count();

            const auto write_started = Clock::now();
            write_all(output.get(), rendered.data(), rendered_bytes);
            write_seconds += std::chrono::duration<double>(Clock::now() - write_started).count();
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
#ifdef V9_STRUCTURED_P8
                  << " p8_selected=" << macro_selected
                  << " p8_completed=" << macro_completed
                  << " p8_candidates=" << macro_candidates
                  << " p8_valid=" << macro_valid
                  << " p8_transitions=" << macro_transitions
                  << " p8_us=" << macro_us_total
#endif
#ifdef V9_PACKED_ATLAS
                  << " atlas_selected=" << atlas_selected
                  << " atlas_completed=" << atlas_completed
#endif
                  << " elapsed=" << elapsed;
        if (options.profile) {
            const double initialization_seconds =
                std::chrono::duration<double>(initialized - started).count();
            std::cerr << " init_s=" << initialization_seconds
                      << " read_s=" << read_seconds
                      << " parse_s=" << parse_seconds
                      << " predict_s=" << predict_seconds
                      << " format_s=" << format_seconds
                      << " write_s=" << write_seconds
#ifdef P26_FUSED_DEFAULT_PIPELINE
                      << " fused_s=" << fused_seconds
#endif
                      ;
        }
#ifdef V9_PACKED_ATLAS
        std::cerr << " algorithm=V9-P14-packed-atlas+compiled-V8\n";
#elif defined(V9_STRUCTURED_P8)
        std::cerr << " algorithm=V9-P13-io+hash+P8-macro+P7-short+bounded-Astar+V8\n";
#elif defined(V9_STRUCTURED_P6)
        std::cerr << " algorithm=V9-P13-io+hash+P7-short+bounded-Astar+V8\n";
#else
        std::cerr << " algorithm=V9-P13-io+hash+bounded-Astar+V8\n";
#endif
        return invalid ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
