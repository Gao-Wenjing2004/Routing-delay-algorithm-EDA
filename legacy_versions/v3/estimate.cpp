#include "srb_hybrid.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using srb_fast::Slice;
using srb_v3::HybridEstimator;

static bool equals_ascii_ci(Slice value, const char* literal) {
    size_t n = 0;
    while (literal[n]) ++n;
    if (value.size != n) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = value.data[i];
        char b = literal[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static void trim(Slice& value) {
    while (value.size && (value.data[0] == ' ' || value.data[0] == '\t' || value.data[0] == '\r')) {
        ++value.data;
        --value.size;
    }
    while (value.size) {
        char c = value.data[value.size - 1];
        if (c != ' ' && c != '\t' && c != '\r') break;
        --value.size;
    }
}

// Contest endpoints contain no commas or quotes, so parsing two views is enough and
// avoids allocating four temporary strings for every one of the 100M requests.
static bool first_two_csv_fields(const std::string& line, Slice& first, Slice& second) {
    size_t c1 = line.find(',');
    if (c1 == std::string::npos) return false;
    size_t c2 = line.find(',', c1 + 1);
    if (c2 == std::string::npos) c2 = line.size();
    first = Slice{line.data(), c1};
    second = Slice{line.data() + c1 + 1, c2 - c1 - 1};
    trim(first);
    trim(second);
    return first.size != 0 && second.size != 0;
}

static void append_slice(std::string& out, Slice value) {
    out.append(value.data, value.size);
}

static void append_uint(std::string& out, uint32_t value) {
    char digits[16];
    char* end = digits + sizeof(digits);
    char* p = end;
    do {
        *--p = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    out.append(p, static_cast<size_t>(end - p));
}

static void usage(const char* exe) {
    std::cerr << "Usage:\n  " << exe
              << " -in input.csv -out output.csv [-atlas local_atlas.bin]\n";
}

static std::string adjacent_path(const char* executable, const char* filename) {
    std::string path = executable ? executable : "";
    size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return filename;
    return path.substr(0, slash + 1) + filename;
}

int main(int argc, char** argv) {
    try {
        std::string input_path;
        std::string output_path;
        std::string atlas_path = adjacent_path(argv[0], "local_atlas.bin");
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "-in" && i + 1 < argc) input_path = argv[++i];
            else if (arg == "-out" && i + 1 < argc) output_path = argv[++i];
            else if (arg == "-atlas" && i + 1 < argc) atlas_path = argv[++i];
            else {
                usage(argv[0]);
                return 2;
            }
        }
        if (input_path.empty() || output_path.empty()) {
            usage(argv[0]);
            return 2;
        }

        std::ifstream input(input_path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open input CSV: " + input_path);
        std::ofstream output(output_path, std::ios::binary);
        if (!output) throw std::runtime_error("cannot open output CSV: " + output_path);

        static std::vector<char> input_buffer(4u << 20);
        static std::vector<char> output_file_buffer(4u << 20);
        input.rdbuf()->pubsetbuf(input_buffer.data(), static_cast<std::streamsize>(input_buffer.size()));
        output.rdbuf()->pubsetbuf(output_file_buffer.data(), static_cast<std::streamsize>(output_file_buffer.size()));

        HybridEstimator estimator(atlas_path);
        std::string pending;
        pending.reserve(4u << 20);
        pending += "From,To,delay\n";

        using clock = std::chrono::steady_clock;
        const auto begin = clock::now();
        uint64_t processed = 0;
        uint64_t bad_rows = 0;
        bool first_nonempty = true;
        std::string line;
        Slice from, to;

        while (std::getline(input, line)) {
            if (line.empty()) continue;
            if (!first_two_csv_fields(line, from, to)) {
                ++bad_rows;
                continue;
            }
            if (first_nonempty && equals_ascii_ci(from, "from") && equals_ascii_ci(to, "to")) {
                first_nonempty = false;
                continue;
            }
            first_nonempty = false;

            uint32_t delay = 0;
            if (!estimator.predict_spec(from, to, delay)) {
                ++bad_rows;
                continue;
            }

            append_slice(pending, from);
            pending.push_back(',');
            append_slice(pending, to);
            pending.push_back(',');
            append_uint(pending, delay);
            pending.push_back('\n');
            ++processed;

            if (pending.size() >= (4u << 20)) {
                output.write(pending.data(), static_cast<std::streamsize>(pending.size()));
                pending.clear();
            }
        }
        if (!pending.empty()) output.write(pending.data(), static_cast<std::streamsize>(pending.size()));
        output.flush();

        const double elapsed = std::chrono::duration<double>(clock::now() - begin).count();
        std::cerr << "processed=" << processed
                  << " bad_rows=" << bad_rows
                  << " local_queries=" << estimator.local_queries()
                  << " fallback_queries=" << estimator.fallback_queries()
                  << " elapsed=" << std::fixed << std::setprecision(3) << elapsed << " s"
                  << " throughput=" << std::setprecision(1)
                  << (elapsed > 0.0 ? processed / elapsed : 0.0) << " q/s\n"
                  << "atlas=" << estimator.atlas_path()
                  << " atlas_memory=" << std::setprecision(1)
                  << (estimator.atlas_memory_bytes() / (1024.0 * 1024.0)) << " MiB\n";
        return bad_rows == 0 ? 0 : 1;
    } catch (const std::exception& exc) {
        std::cerr << "error: " << exc.what() << '\n';
        return 1;
    }
}
