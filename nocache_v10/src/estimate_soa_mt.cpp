#include "../../submission_k48_20261015/src/estimator.hpp"
#include "../../submission_k48_20261015/src/io_fast.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using srb_fast::Slice;
using Clock = std::chrono::steady_clock;

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
    for (size_t index = 0; index < value.size; ++index) {
        const char c = value.data[index] >= 'A' && value.data[index] <= 'Z'
            ? static_cast<char>(value.data[index] + 32) : value.data[index];
        if (c != expected[index]) return false;
    }
    return true;
}

void write_all(FILE* stream, const char* data, size_t bytes) {
    if (bytes && std::fwrite(data, 1, bytes, stream) != bytes)
        throw std::runtime_error("output write failed");
}

double seconds(Clock::duration duration) {
    return std::chrono::duration<double>(duration).count();
}

struct EndpointSoA {
    std::vector<Slice> from;
    std::vector<Slice> to;
    std::vector<uint16_t> ax, ay, ap, bx, by, bp;

    void clear() {
        from.clear(); to.clear(); ax.clear(); ay.clear(); ap.clear(); bx.clear(); by.clear(); bp.clear();
    }
    void reserve(size_t rows) {
        from.reserve(rows); to.reserve(rows); ax.reserve(rows); ay.reserve(rows); ap.reserve(rows);
        bx.reserve(rows); by.reserve(rows); bp.reserve(rows);
    }
    void push(Slice first, Slice second, const srb_fast::Endpoint& a, const srb_fast::Endpoint& b) {
        from.push_back(first); to.push_back(second);
        ax.push_back(static_cast<uint16_t>(a.x)); ay.push_back(static_cast<uint16_t>(a.y));
        ap.push_back(static_cast<uint16_t>(a.port)); bx.push_back(static_cast<uint16_t>(b.x));
        by.push_back(static_cast<uint16_t>(b.y)); bp.push_back(static_cast<uint16_t>(b.port));
    }
    size_t size() const { return from.size(); }
};

struct Result {
    uint32_t delay = 0;
    uint8_t kind = 0;
};

}  // namespace

int main(int argc, char** argv) {
    const auto total_started = Clock::now();
    try {
        std::string input_path, output_path;
        std::string atlas_path =
            (std::filesystem::path(argv[0]).parent_path() / "assets/lower_k32.bin").string();
        int local_radius = 24;
        unsigned threads = 1;
        bool use_atlas = true;
        for (int index = 1; index < argc; ++index) {
            const std::string option = argv[index];
            if (option == "-in" && index + 1 < argc) input_path = argv[++index];
            else if (option == "-out" && index + 1 < argc) output_path = argv[++index];
            else if (option == "-atlas" && index + 1 < argc) atlas_path = argv[++index];
            else if (option == "-local-radius" && index + 1 < argc) local_radius = std::stoi(argv[++index]);
            else if (option == "-radius" && index + 1 < argc) {
                const int radius = std::stoi(argv[++index]);
                if (radius != 0 && radius != 72) throw std::runtime_error("radius must be 0 or 72");
                use_atlas = radius != 0;
            } else if (option == "-threads" && index + 1 < argc) {
                threads = static_cast<unsigned>(std::stoul(argv[++index]));
            } else {
                throw std::runtime_error("usage: estimate_soa_mt -in request.csv -out result.csv [-threads 1|2|4|8]");
            }
        }
        if (input_path.empty() || output_path.empty() ||
            (threads != 1 && threads != 2 && threads != 4 && threads != 8))
            throw std::runtime_error("input/output required; threads must be 1, 2, 4 or 8");
        srb_v11_io::protect_same_file(input_path, output_path);

        using File = std::unique_ptr<FILE, decltype(&std::fclose)>;
        File input(std::fopen(input_path.c_str(), "rb"), &std::fclose);
        File output(std::fopen(output_path.c_str(), "wb"), &std::fclose);
        if (!input || !output) throw std::runtime_error("cannot open input/output");
        std::setvbuf(input.get(), nullptr, _IOFBF, 1 << 20);
        std::setvbuf(output.get(), nullptr, _IOFBF, 1 << 20);

        const auto load_started = Clock::now();
        srb_v11_hybrid::Estimator estimator(atlas_path, local_radius, use_atlas);
        DirectPortParser parser;
        const double load_seconds = seconds(Clock::now() - load_started);

        constexpr size_t block = 16u << 20;
        constexpr size_t tile = 32;
        std::vector<char> buffer(block + 4096);
        srb_v11_io::OutputBuffer text(block);
        EndpointSoA rows;
        rows.reserve(block / 24);
        std::vector<Result> results;
        size_t carry = 0;
        bool first_line = true;
        uint64_t count = 0, bad = 0;
        std::array<uint64_t, 5> branches{};
        double read_seconds = 0, parse_seconds = 0, predict_seconds = 0;
        double format_seconds = 0, write_seconds = 0;

        write_all(output.get(), "From,To,Delay\n", sizeof("From,To,Delay\n") - 1);
        while (true) {
            auto stage = Clock::now();
            const size_t bytes = std::fread(buffer.data() + carry, 1, block, input.get());
            read_seconds += seconds(Clock::now() - stage);
            if (std::ferror(input.get())) throw std::runtime_error("input read failed");
            const bool eof = bytes < block;
            const size_t total = carry + bytes;
            if (!total) break;

            stage = Clock::now();
            rows.clear();
            size_t position = 0;
            while (position < total) {
                const char* end = static_cast<const char*>(
                    std::memchr(buffer.data() + position, '\n', total - position));
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
                    Slice header_a, header_b;
                    if (fields(line, header_a, header_b) && equal_ignore_case(header_a, "from") &&
                        equal_ignore_case(header_b, "to"))
                        continue;
                }
                Slice from, to;
                srb_fast::Endpoint a, b;
                if (!fields(line, from, to) || !parser.parse_endpoint(from, a) ||
                    !parser.parse_endpoint(to, b)) {
                    ++bad;
                    continue;
                }
                rows.push(from, to, a, b);
            }
            parse_seconds += seconds(Clock::now() - stage);

            stage = Clock::now();
            results.resize(rows.size());
            const unsigned workers = static_cast<unsigned>(
                std::min<size_t>(threads, std::max<size_t>(rows.size(), 1)));
            std::vector<std::array<uint64_t, 5>> worker_branches(workers);
            auto work = [&](unsigned worker) {
                const size_t begin = rows.size() * worker / workers;
                const size_t end = rows.size() * (worker + 1) / workers;
                for (size_t tile_begin = begin; tile_begin < end; tile_begin += tile) {
                    const size_t tile_end = std::min(end, tile_begin + tile);
                    std::array<srb_v11_hybrid::Estimator::PreparedRequest, tile> prepared;
                    for (size_t index = tile_begin; index < tile_end; ++index) {
                        const srb_fast::Endpoint a{static_cast<int16_t>(rows.ax[index]),
                                                   static_cast<int16_t>(rows.ay[index]), rows.ap[index]};
                        const srb_fast::Endpoint b{static_cast<int16_t>(rows.bx[index]),
                                                   static_cast<int16_t>(rows.by[index]), rows.bp[index]};
                        estimator.prepare_endpoints(a, b, prepared[index - tile_begin]);
                    }
                    for (size_t index = tile_begin; index < tile_end; ++index) {
                        Result& result = results[index];
                        result.delay = estimator.predict_prepared(prepared[index - tile_begin], result.kind);
                        ++worker_branches[worker][result.kind];
                    }
                }
            };
            std::vector<std::thread> pool;
            pool.reserve(workers > 0 ? workers - 1 : 0);
            for (unsigned worker = 1; worker < workers; ++worker) pool.emplace_back(work, worker);
            work(0);
            for (auto& thread : pool) thread.join();
            for (const auto& local : worker_branches)
                for (size_t index = 0; index < branches.size(); ++index) branches[index] += local[index];
            predict_seconds += seconds(Clock::now() - stage);

            stage = Clock::now();
            text.clear();
            for (size_t index = 0; index < rows.size(); ++index)
                text.row(rows.from[index], rows.to[index], results[index].delay);
            format_seconds += seconds(Clock::now() - stage);

            stage = Clock::now();
            write_all(output.get(), text.data(), text.size());
            write_seconds += seconds(Clock::now() - stage);
            count += rows.size();

            carry = total - position;
            if (carry > 4096) throw std::runtime_error("CSV row exceeds 4096 bytes");
            if (carry) std::memmove(buffer.data(), buffer.data() + position, carry);
            if (eof) break;
        }
        const auto flush_started = Clock::now();
        if (std::fflush(output.get())) throw std::runtime_error("output flush failed");
        write_seconds += seconds(Clock::now() - flush_started);
        const double total_seconds = seconds(Clock::now() - total_started);
        std::cerr << "processed=" << count << " bad_rows=" << bad << " threads=" << threads
                  << " total_elapsed=" << total_seconds << " load=" << load_seconds
                  << " read=" << read_seconds << " parse=" << parse_seconds
                  << " predict=" << predict_seconds << " format=" << format_seconds
                  << " write=" << write_seconds << " atlas_memory_bytes=" << estimator.atlas_memory_bytes()
                  << " branches=" << branches[0] << ':' << branches[1] << ':' << branches[2] << ':'
                  << branches[3] << ':' << branches[4] << '\n';
        return bad ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
