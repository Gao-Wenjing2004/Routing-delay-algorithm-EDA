#include "../../submission_k48_20261015/src/estimator.hpp"
#include "../../submission_k48_20261015/src/io_fast.hpp"
#include "direct_residual.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef SRB_BATCH_WIDTH
#define SRB_BATCH_WIDTH 4
#endif
static_assert(SRB_BATCH_WIDTH == 4 || SRB_BATCH_WIDTH == 8, "batch width must be 4 or 8");

using srb_fast::Slice;

namespace {
void trim(Slice& value) {
    while (value.size && (value.data[0] == ' ' || value.data[0] == '\t' || value.data[0] == '\r')) {
        ++value.data;
        --value.size;
    }
    while (value.size && (value.data[value.size - 1] == ' ' || value.data[value.size - 1] == '\t' ||
                          value.data[value.size - 1] == '\r'))
        --value.size;
}

bool fields(Slice line, Slice& first, Slice& second) {
    const char* comma = static_cast<const char*>(std::memchr(line.data, ',', line.size));
    if (!comma) return false;
    first = {line.data, static_cast<size_t>(comma - line.data)};
    second = {comma + 1, line.size - first.size - 1};
    if (const char* last = static_cast<const char*>(std::memchr(second.data, ',', second.size)))
        second.size = static_cast<size_t>(last - second.data);
    trim(first);
    trim(second);
    return first.size && second.size;
}

bool equal_ignore_case(Slice value, const char* expected) {
    if (value.size != std::strlen(expected)) return false;
    for (size_t i = 0; i < value.size; ++i) {
        const char current = value.data[i] >= 'A' && value.data[i] <= 'Z' ? value.data[i] + 32 : value.data[i];
        if (current != expected[i]) return false;
    }
    return true;
}

void write_all(FILE* stream, const char* data, size_t bytes) {
    if (bytes && std::fwrite(data, 1, bytes, stream) != bytes) throw std::runtime_error("write failed");
}

void append_slice(std::string& output, Slice value) { output.append(value.data, value.size); }

void append_uint(std::string& output, uint32_t value) {
    char buffer[16];
    char* end = srb_v11_io::integer(buffer, value);
    output.append(buffer, static_cast<size_t>(end - buffer));
}

void append_int(std::string& output, int value) {
    if (value < 0) {
        output.push_back('-');
        append_uint(output, static_cast<uint32_t>(-static_cast<int64_t>(value)));
    } else {
        append_uint(output, static_cast<uint32_t>(value));
    }
}

const char* branch_name(const srb_v11_hybrid::Estimator::PreparedRequest& query, uint8_t kind) {
    if (query.state == 1) return "identity";
    if (query.state == 2) return "unreachable";
    if (kind == 1) return "local_r24";
    if (kind == 2) return query.lng ? "long_hull" : "safe_hull";
    if (kind == 4) {
        if (query.short48_valid) return "short48_or_parent";
        if (query.short_valid || query.short_deferred) return "short_teacher";
        return query.lng ? "long_dual" : "short_dual";
    }
    return "normalized_fallback";
}

unsigned branch_id(const srb_v11_hybrid::Estimator::PreparedRequest& query, uint8_t kind) {
    if (query.state == 1) return 0;
    if (query.state == 2) return 1;
    if (kind == 1) return 2;
    if (kind == 2) return query.lng ? 4 : 3;
    if (kind == 4) {
        if (query.short48_valid) return 5;
        if (query.short_valid || query.short_deferred) return 6;
        return query.lng ? 7 : 8;
    }
    return 9;
}

unsigned block_mask(const srb_v11_hybrid::Estimator::PreparedRequest& query) {
    if (query.state == 1 || query.state == 2) return 0;
    const int min_x = std::min<int>(query.n.x, query.b.x);
    const int max_x = std::max<int>(query.n.x, query.b.x);
    const int min_y = std::min<int>(query.n.y, query.b.y);
    const int max_y = std::max<int>(query.n.y, query.b.y);
    unsigned mask = 0;
    for (int index = 0; index < srb_fast_data::kBlockCount; ++index) {
        if (min_x <= srb_fast_data::kBlockRight[index] && max_x >= srb_fast_data::kBlockLeft[index] &&
            min_y <= srb_fast_data::kBlockUpper[index] && max_y >= srb_fast_data::kBlockLower[index])
            mask |= 1u << index;
    }
    return mask;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        std::string input_path, output_path, trace_path, residual_path;
        std::string atlas_path =
            (std::filesystem::path(argv[0]).parent_path() / "assets/lower_k32.bin").string();
        int local_radius = 24;
        for (int index = 1; index < argc; ++index) {
            const std::string option = argv[index];
            if (option == "-in" && index + 1 < argc) input_path = argv[++index];
            else if (option == "-out" && index + 1 < argc) output_path = argv[++index];
            else if (option == "-trace" && index + 1 < argc) trace_path = argv[++index];
            else if (option == "-atlas" && index + 1 < argc) atlas_path = argv[++index];
            else if (option == "-residual" && index + 1 < argc) residual_path = argv[++index];
            else if (option == "-local-radius" && index + 1 < argc) local_radius = std::stoi(argv[++index]);
            else throw std::runtime_error("usage: estimate_trace -in request.csv -out result.csv -trace trace.csv -atlas lower_k32.bin");
        }
        if (input_path.empty() || output_path.empty())
            throw std::runtime_error("input and output are required");
        srb_v11_io::protect_same_file(input_path, output_path);
        if (!trace_path.empty()) srb_v11_io::protect_same_file(input_path, trace_path);
        using File = std::unique_ptr<FILE, decltype(&std::fclose)>;
        File input(std::fopen(input_path.c_str(), "rb"), &std::fclose);
        File output(std::fopen(output_path.c_str(), "wb"), &std::fclose);
        File trace(trace_path.empty() ? nullptr : std::fopen(trace_path.c_str(), "wb"), &std::fclose);
        if (!input || !output || (!trace_path.empty() && !trace))
            throw std::runtime_error("cannot open input/output/trace");
        srb_v11_hybrid::Estimator estimator(atlas_path, local_radius, true);
        std::unique_ptr<nocache_v10::DirectResidual> residual;
        if (residual_path.empty()) {
            const auto candidate = std::filesystem::path(argv[0]).parent_path() / "assets/direct_residual.bin";
            if (std::filesystem::exists(candidate)) residual_path = candidate.string();
        }
        if (!residual_path.empty()) residual = std::make_unique<nocache_v10::DirectResidual>(residual_path);
        std::setvbuf(input.get(), nullptr, _IOFBF, 1 << 20);
        std::setvbuf(output.get(), nullptr, _IOFBF, 1 << 20);
        if (trace) std::setvbuf(trace.get(), nullptr, _IOFBF, 1 << 20);
        write_all(output.get(), "From,To,Delay\n", 14);
        static constexpr char trace_header[] =
            "From,To,Delay,BaseDelay,Correction,Branch,SourcePort,TargetPort,Dx,Dy,Gap,BlockMask\n";
        if (trace) write_all(trace.get(), trace_header, sizeof(trace_header) - 1);

        constexpr size_t block = 4u << 20;
        constexpr unsigned batch_width = SRB_BATCH_WIDTH;
        std::vector<char> buffer(block + 4096);
        srb_v11_io::OutputBuffer result_text(block);
        std::string trace_text;
        trace_text.reserve(block);
        struct Pending {
            Slice from, to;
            srb_v11_hybrid::Estimator::PreparedRequest query;
        };
        std::array<Pending, batch_width> pending{};
        unsigned pending_count = 0;
        uint64_t rows = 0, bad = 0;
        size_t carry = 0;
        bool first_line = true;
        auto flush_batch = [&] {
            for (unsigned index = 0; index < pending_count; ++index) {
                uint8_t kind = 0;
                const Pending& row = pending[index];
                const uint32_t base_delay = estimator.predict_prepared(row.query, kind);
                const unsigned blocks = block_mask(row.query);
                const bool normalized = row.query.state != 1 && row.query.state != 2;
                const int dx = normalized ? row.query.dx : 0;
                const int dy = normalized ? row.query.dy : 0;
                const int gap = normalized && (row.query.state == 3 || row.query.lng) ? row.query.gap : 0;
                const int correction = residual ? residual->correction(
                    branch_id(row.query, kind), dx, dy, gap, blocks) : 0;
                const int64_t adjusted = static_cast<int64_t>(base_delay) + correction;
                const uint32_t delay = static_cast<uint32_t>(std::max<int64_t>(0, adjusted));
                result_text.row(row.from, row.to, delay);
                if (trace) {
                append_slice(trace_text, row.from);
                trace_text.push_back(',');
                append_slice(trace_text, row.to);
                trace_text.push_back(',');
                append_uint(trace_text, delay);
                trace_text.push_back(',');
                append_uint(trace_text, base_delay);
                trace_text.push_back(',');
                append_int(trace_text, correction);
                trace_text.push_back(',');
                trace_text.append(branch_name(row.query, kind));
                trace_text.push_back(',');
                append_uint(trace_text, row.query.a.port);
                trace_text.push_back(',');
                append_uint(trace_text, row.query.b.port);
                trace_text.push_back(',');
                append_int(trace_text, dx);
                trace_text.push_back(',');
                append_int(trace_text, dy);
                trace_text.push_back(',');
                append_int(trace_text, gap);
                trace_text.push_back(',');
                append_uint(trace_text, blocks);
                trace_text.push_back('\n');
                }
                ++rows;
            }
            pending_count = 0;
        };

        while (true) {
            const size_t bytes = std::fread(buffer.data() + carry, 1, block, input.get());
            if (std::ferror(input.get())) throw std::runtime_error("input read failed");
            const bool eof = bytes < block;
            const size_t total = carry + bytes;
            if (!total) break;
            size_t position = 0;
            result_text.clear();
            trace_text.clear();
            while (position < total) {
                const char* end = static_cast<const char*>(std::memchr(buffer.data() + position, '\n', total - position));
                if (!end && !eof) break;
                const size_t stop = end ? static_cast<size_t>(end - buffer.data()) : total;
                Slice line{buffer.data() + position, stop - position};
                position = end ? stop + 1 : total;
                if (!line.size) continue;
                if (first_line) {
                    first_line = false;
                    if (line.size >= 3 && std::memcmp(line.data, "\xef\xbb\xbf", 3) == 0) {
                        line.data += 3;
                        line.size -= 3;
                    }
                    Slice a, b;
                    if (fields(line, a, b) && equal_ignore_case(a, "from") && equal_ignore_case(b, "to")) continue;
                }
                Slice from, to;
                Pending& row = pending[pending_count];
                if (!fields(line, from, to) || !estimator.prepare_spec(from, to, row.query)) {
                    ++bad;
                    continue;
                }
                row.from = from;
                row.to = to;
                if (++pending_count == batch_width) flush_batch();
            }
            flush_batch();
            write_all(output.get(), result_text.data(), result_text.size());
            if (trace) write_all(trace.get(), trace_text.data(), trace_text.size());
            carry = total - position;
            if (carry > 4096) throw std::runtime_error("CSV row exceeds 4096 bytes");
            if (carry) std::memmove(buffer.data(), buffer.data() + position, carry);
            if (eof) break;
        }
        if (std::fflush(output.get()) || (trace && std::fflush(trace.get())))
            throw std::runtime_error("flush failed");
        std::cerr << "processed=" << rows << " bad_rows=" << bad
                  << " atlas_memory_bytes=" << estimator.atlas_memory_bytes()
                  << " residual_bytes=" << (residual ? residual->memory_bytes() : 0) << '\n';
        return bad ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
