#include "p2_estimator.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

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

void append_number(std::string& output, uint32_t value) {
    char digits[16];
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

}  // namespace

int main(int argc, char** argv) {
    const auto started = std::chrono::steady_clock::now();
    try {
        std::string input_path, output_path;
        int threads = 1;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "-in" && i + 1 < argc) input_path = argv[++i];
            else if (argument == "-out" && i + 1 < argc) output_path = argv[++i];
            else if (argument == "-threads" && i + 1 < argc) threads = std::stoi(argv[++i]);
            else throw std::runtime_error("usage: estimate -in request.csv -out result.csv [-threads 1]");
        }
        if (input_path.empty() || output_path.empty() || threads != 1)
            throw std::runtime_error("-in and -out are required; V8 P2 is deterministic single-threaded code");
        if (std::filesystem::absolute(input_path).lexically_normal() ==
            std::filesystem::absolute(output_path).lexically_normal())
            throw std::runtime_error("input and output must differ");

        using File = std::unique_ptr<FILE, decltype(&std::fclose)>;
        File input(std::fopen(input_path.c_str(), "rb"), &std::fclose);
        File output(std::fopen(output_path.c_str(), "wb"), &std::fclose);
        if (!input || !output) throw std::runtime_error("cannot open input or output");
        std::setvbuf(input.get(), nullptr, _IOFBF, 1 << 20);
        std::setvbuf(output.get(), nullptr, _IOFBF, 1 << 20);

        p2_runtime::Estimator estimator;
        constexpr std::size_t block_size = 4u << 20;
        std::vector<char> buffer(block_size + 4096);
        std::string rendered;
        rendered.reserve(block_size + block_size / 4);
        write_all(output.get(), "From,To,Delay\n", sizeof("From,To,Delay\n") - 1);
        std::size_t carry = 0;
        bool first = true;
        uint64_t processed = 0, invalid = 0;
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
                    srb_fast::Slice from, to;
                    if (fields(line, from, to) && case_equal(from, "from") && case_equal(to, "to")) continue;
                }
                srb_fast::Slice from, to;
                uint32_t delay = 0;
                if (!fields(line, from, to) || !estimator.predict_spec(from, to, delay)) {
                    ++invalid;
                    continue;
                }
                rendered.append(from.data, from.size);
                rendered.push_back(',');
                rendered.append(to.data, to.size);
                rendered.push_back(',');
                append_number(rendered, delay);
                rendered.push_back('\n');
                ++processed;
            }
            write_all(output.get(), rendered.data(), rendered.size());
            carry = total - position;
            if (carry > 4096) throw std::runtime_error("CSV row exceeds 4096 bytes");
            if (carry) std::memmove(buffer.data(), buffer.data() + position, carry);
            if (eof) break;
        }
        if (std::fflush(output.get()) != 0) throw std::runtime_error("output flush failed");
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        std::cerr << "processed=" << processed << " invalid_rows=" << invalid
                  << " elapsed=" << elapsed << " algorithm=PRP-R-P2-composite\n";
        return invalid ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
