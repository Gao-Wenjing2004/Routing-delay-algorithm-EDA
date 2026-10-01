#include "srb_v4.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using srb_fast::Slice;

namespace {

bool equals_ascii_ci(Slice value, const char* literal) {
    size_t size = 0;
    while (literal[size]) ++size;
    if (value.size != size) return false;
    for (size_t index = 0; index < size; ++index) {
        char lhs = value.data[index];
        char rhs = literal[index];
        if (lhs >= 'A' && lhs <= 'Z') lhs = static_cast<char>(lhs - 'A' + 'a');
        if (rhs >= 'A' && rhs <= 'Z') rhs = static_cast<char>(rhs - 'A' + 'a');
        if (lhs != rhs) return false;
    }
    return true;
}

void trim(Slice& value) {
    while (value.size &&
           (value.data[0] == ' ' || value.data[0] == '\t' || value.data[0] == '\r')) {
        ++value.data;
        --value.size;
    }
    while (value.size) {
        const char c = value.data[value.size - 1];
        if (c != ' ' && c != '\t' && c != '\r') break;
        --value.size;
    }
}

bool first_two_csv_fields(const std::string& line, Slice& first, Slice& second) {
    const size_t comma1 = line.find(',');
    if (comma1 == std::string::npos) return false;
    size_t comma2 = line.find(',', comma1 + 1);
    if (comma2 == std::string::npos) comma2 = line.size();
    first = Slice{line.data(), comma1};
    second = Slice{line.data() + comma1 + 1, comma2 - comma1 - 1};
    trim(first);
    trim(second);
    return first.size != 0 && second.size != 0;
}

void append_slice(std::string& output, Slice value) { output.append(value.data, value.size); }

void append_uint(std::string& output, uint32_t value) {
    char digits[16];
    char* end = digits + sizeof(digits);
    char* current = end;
    do {
        *--current = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    output.append(current, static_cast<size_t>(end - current));
}

std::string adjacent_path(const char* executable, const char* filename) {
    const std::string path = executable ? executable : "";
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return filename;
    return path.substr(0, slash + 1) + filename;
}

void usage(const char* executable) {
    std::cerr
        << "Usage:\n  " << executable
        << " -in input.csv -out output.csv [-atlas local_atlas.bin]"
           " [--mode v3|v4|v4-base"
#ifdef SRB_V6_HAS_MACRO
           "|v6"
#endif
           "] [--residual-kernel reference|optimized"
#ifdef SRB_V4_HAS_FAMILY_RESIDUAL
           "|family"
#endif
           "]"
           " [--atlas-query-radius 0..56]"
           " [--disable-atlas] [--skip-atlas-fingerprint-check]\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        using clock = std::chrono::steady_clock;
        const auto process_begin = clock::now();
        std::string input_path;
        std::string output_path;
        std::string atlas_path = adjacent_path(argv[0], "local_atlas.bin");
        bool residual = true;
        bool fused_fallback = true;
        bool atlas = true;
        bool verify_atlas = true;
#ifdef SRB_V6_DEFAULT_ATLAS_QUERY_RADIUS
        bool atlas_query_radius_explicit = false;
#endif
        std::string selected_mode =
#ifdef SRB_V6_HAS_MACRO
            "v6";
#else
            "v4";
#endif
        int atlas_query_radius =
#ifdef SRB_V6_DEFAULT_ATLAS_QUERY_RADIUS
            SRB_V6_DEFAULT_ATLAS_QUERY_RADIUS;
#else
            static_cast<int>(srb_v4::kQueryRadius);
#endif
        srb_v4::ResidualKernel residual_kernel =
#ifdef SRB_V6_HAS_MACRO
            srb_v4::ResidualKernel::Family;
#else
            srb_v4::ResidualKernel::Optimized;
#endif
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "-in" && index + 1 < argc) input_path = argv[++index];
            else if (argument == "-out" && index + 1 < argc) output_path = argv[++index];
            else if (argument == "-atlas" && index + 1 < argc) atlas_path = argv[++index];
            else if (argument == "--mode" && index + 1 < argc) {
                const std::string mode = argv[++index];
                selected_mode = mode;
                if (mode == "v3") {
                    residual = false;
                    fused_fallback = false;
                } else if (mode == "v4") {
                    residual = true;
                    fused_fallback = true;
                } else if (mode == "v4-base") {
                    residual = false;
                    fused_fallback = true;
#ifdef SRB_V6_HAS_MACRO
                } else if (mode == "v6") {
                    residual = true;
                    fused_fallback = true;
                    residual_kernel = srb_v4::ResidualKernel::Family;
#endif
                } else {
                    throw std::runtime_error("unsupported --mode for this build");
                }
            } else if (argument == "--disable-atlas") atlas = false;
            else if (argument == "--atlas-query-radius" && index + 1 < argc) {
                atlas_query_radius = std::stoi(argv[++index]);
#ifdef SRB_V6_DEFAULT_ATLAS_QUERY_RADIUS
                atlas_query_radius_explicit = true;
#endif
            }
            else if (argument == "--residual-kernel" && index + 1 < argc) {
                const std::string kernel = argv[++index];
                if (kernel == "reference") {
                    residual_kernel = srb_v4::ResidualKernel::Reference;
                } else if (kernel == "optimized") {
                    residual_kernel = srb_v4::ResidualKernel::Optimized;
#ifdef SRB_V4_HAS_FAMILY_RESIDUAL
                } else if (kernel == "family") {
                    residual_kernel = srb_v4::ResidualKernel::Family;
#endif
                } else {
                    throw std::runtime_error(
                        "unsupported --residual-kernel for this build");
                }
            }
            else if (argument == "--skip-atlas-fingerprint-check") verify_atlas = false;
            else {
                usage(argv[0]);
                return 2;
            }
        }
        if (input_path.empty() || output_path.empty()) {
            usage(argv[0]);
            return 2;
        }
#ifdef SRB_V6_DEFAULT_ATLAS_QUERY_RADIUS
        if (!atlas_query_radius_explicit) {
            atlas_query_radius = selected_mode == "v6"
                                     ? SRB_V6_DEFAULT_ATLAS_QUERY_RADIUS
                                     : static_cast<int>(srb_v4::kQueryRadius);
        }
#endif

        const auto load_begin = clock::now();
        srb_v4::Estimator estimator(
            atlas_path, atlas, residual, verify_atlas, fused_fallback, residual_kernel,
            atlas_query_radius);
        const auto load_end = clock::now();
        std::ifstream input(input_path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open input CSV: " + input_path);
        std::ofstream output(output_path, std::ios::binary);
        if (!output) throw std::runtime_error("cannot open output CSV: " + output_path);
        static std::vector<char> input_buffer(4u << 20);
        static std::vector<char> output_buffer(4u << 20);
        input.rdbuf()->pubsetbuf(
            input_buffer.data(), static_cast<std::streamsize>(input_buffer.size()));
        output.rdbuf()->pubsetbuf(
            output_buffer.data(), static_cast<std::streamsize>(output_buffer.size()));

        std::string pending;
        pending.reserve(4u << 20);
        pending += "From,To,delay\n";
        uint64_t processed = 0;
        uint64_t bad_rows = 0;
        uint64_t line_number = 0;
        bool first_nonempty = true;
        std::string line;
        Slice from, to;
        double write_seconds = 0.0;
        const auto query_begin = clock::now();
        while (std::getline(input, line)) {
            ++line_number;
            if (line.empty()) continue;
            if (!first_two_csv_fields(line, from, to)) {
                ++bad_rows;
                throw std::runtime_error("invalid CSV at line " + std::to_string(line_number));
            }
            if (first_nonempty && equals_ascii_ci(from, "from") && equals_ascii_ci(to, "to")) {
                first_nonempty = false;
                continue;
            }
            first_nonempty = false;
            srb_v4::Prediction prediction;
            if (!estimator.predict_spec(from, to, prediction)) {
                ++bad_rows;
                throw std::runtime_error("invalid endpoint at line " + std::to_string(line_number));
            }
            append_slice(pending, from);
            pending.push_back(',');
            append_slice(pending, to);
            pending.push_back(',');
            append_uint(pending, prediction.delay);
            pending.push_back('\n');
            ++processed;
            if (pending.size() >= (4u << 20)) {
                const auto write_begin = clock::now();
                output.write(pending.data(), static_cast<std::streamsize>(pending.size()));
                write_seconds += std::chrono::duration<double>(clock::now() - write_begin).count();
                pending.clear();
            }
        }
        if (!pending.empty()) {
            const auto write_begin = clock::now();
            output.write(pending.data(), static_cast<std::streamsize>(pending.size()));
            output.flush();
            write_seconds += std::chrono::duration<double>(clock::now() - write_begin).count();
        }
        const auto query_end = clock::now();
        const double load_seconds = std::chrono::duration<double>(load_end - load_begin).count();
        const double query_seconds = std::chrono::duration<double>(query_end - query_begin).count();
        const double total_seconds = std::chrono::duration<double>(query_end - process_begin).count();
        std::cerr << "processed=" << processed
                  << " bad_rows=" << bad_rows
                  << " same_endpoint_queries=" << estimator.same_endpoint_queries()
                  << " legacy_atlas_queries=" << estimator.atlas_queries()
                  << " v3_fallback_queries=" << estimator.fallback_queries()
                  << " v4_residual_queries=" << estimator.residual_queries()
                  << " atlas_load=" << std::fixed << std::setprecision(6) << load_seconds << " s"
                  << " query_and_io=" << query_seconds << " s"
                  << " buffered_write=" << write_seconds << " s"
                  << " total=" << total_seconds << " s"
                  << " throughput=" << std::setprecision(1)
                  << (query_seconds > 0.0 ? processed / query_seconds : 0.0) << " q/s\n"
                  << "mode="
#ifdef SRB_V6_HAS_MACRO
                  << (residual && residual_kernel == srb_v4::ResidualKernel::Family
                          ? "v6"
                          : (residual ? "v4" : (fused_fallback ? "v4-base" : "v3")))
#else
                  << (residual ? "v4" : (fused_fallback ? "v4-base" : "v3"))
#endif
                  << " atlas_enabled=" << atlas
                  << " atlas_query_radius=" << atlas_query_radius
                  << " atlas_verified=" << (atlas && verify_atlas)
                  << " atlas_memory=" << std::setprecision(1)
                  << (estimator.atlas_memory_bytes() / (1024.0 * 1024.0)) << " MiB"
                  << " residual_parameters="
#ifdef SRB_V4_HAS_FAMILY_RESIDUAL
                  << (residual_kernel == srb_v4::ResidualKernel::Family
                          ? srb_v4_family_data::kParameterCount
                          : srb_v4_data::kParameterCount)
#else
                  << srb_v4_data::kParameterCount
#endif
                  << " residual_kernel="
                  << (residual_kernel == srb_v4::ResidualKernel::Optimized ? "optimized" :
#ifdef SRB_V4_HAS_FAMILY_RESIDUAL
                      residual_kernel == srb_v4::ResidualKernel::Family ? "family" :
#endif
                      "reference")
                  << " feature_version="
#ifdef SRB_V4_HAS_FAMILY_RESIDUAL
                  << (residual_kernel == srb_v4::ResidualKernel::Family
                          ? srb_v4_family_data::kFeatureVersion
                          : srb_v4_data::kFeatureVersion)
#else
                  << srb_v4_data::kFeatureVersion
#endif
                  << " architecture_sha256=" << srb_v4_data::kArchitectureSha256 << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "error: " << exception.what() << '\n';
        return 1;
    }
}
