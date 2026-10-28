#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr uint16_t kInf = 65535;

template <class T>
T read_value(std::istream& stream) {
    T value{};
    if (!stream.read(reinterpret_cast<char*>(&value), sizeof(value)))
        throw std::runtime_error("truncated Portal matrix");
    return value;
}

template <class T>
void write_value(std::ostream& stream, T value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

struct MatrixFile {
    uint16_t dimension = 0;
    uint16_t band = 0;
    int8_t direction = 0;
    std::vector<uint16_t> costs;
};

MatrixFile load(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    char magic[8];
    if (!stream.read(magic, sizeof(magic)) || std::memcmp(magic, "PPORT001", 8) != 0)
        throw std::runtime_error("bad Portal matrix: " + path);
    const uint16_t version = read_value<uint16_t>(stream);
    MatrixFile result;
    result.dimension = read_value<uint16_t>(stream);
    result.band = read_value<uint16_t>(stream);
    const uint16_t periods = read_value<uint16_t>(stream);
    result.direction = read_value<int8_t>(stream);
    char padding[3];
    if (!stream.read(padding, sizeof(padding))) throw std::runtime_error("truncated header");
    if (version != 1 || periods != 1 || result.dimension == 0 ||
        (result.direction != 1 && result.direction != -1))
        throw std::runtime_error("unsupported Portal matrix header");
    result.costs.resize(static_cast<size_t>(result.dimension) * result.dimension);
    if (!stream.read(reinterpret_cast<char*>(result.costs.data()),
                     static_cast<std::streamsize>(result.costs.size() * sizeof(uint16_t))))
        throw std::runtime_error("truncated Portal matrix body");
    return result;
}

std::vector<uint16_t> compose(
    const std::vector<uint16_t>& left,
    const std::vector<uint16_t>& right,
    int dimension,
    uint64_t& operations) {
    const size_t entries = static_cast<size_t>(dimension) * dimension;
    std::vector<uint16_t> output(entries, kInf);
    for (int source = 0; source < dimension; ++source) {
        uint16_t* destination = output.data() + static_cast<size_t>(source) * dimension;
        const uint16_t* left_row = left.data() + static_cast<size_t>(source) * dimension;
        for (int middle = 0; middle < dimension; ++middle) {
            const uint16_t first = left_row[middle];
            if (first == kInf) continue;
            const uint16_t* right_row = right.data() + static_cast<size_t>(middle) * dimension;
            for (int target = 0; target < dimension; ++target) {
                const uint16_t second = right_row[target];
                if (second == kInf) continue;
                ++operations;
                const uint32_t total = static_cast<uint32_t>(first) + second;
                if (total < destination[target])
                    destination[target] = static_cast<uint16_t>(total);
            }
        }
    }
    return output;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 4)
            throw std::runtime_error("usage: offline_portal_closure input.bin output.bin max_periods");
        const MatrixFile base = load(argv[1]);
        const int max_periods = std::stoi(argv[3]);
        if (max_periods < 1 || max_periods > 5)
            throw std::runtime_error("max_periods must be in [1,5]");

        std::ofstream output(argv[2], std::ios::binary);
        if (!output) throw std::runtime_error("cannot create output");
        output.write("PPCLOS01", 8);
        write_value<uint16_t>(output, 1);
        write_value<uint16_t>(output, base.dimension);
        write_value<uint16_t>(output, base.band);
        write_value<uint16_t>(output, static_cast<uint16_t>(max_periods));
        write_value<int8_t>(output, base.direction);
        const char padding[3]{};
        output.write(padding, sizeof(padding));

        const auto started = std::chrono::steady_clock::now();
        std::vector<uint16_t> current = base.costs;
        uint64_t operations = 0;
        for (int period = 1; period <= max_periods; ++period) {
            output.write(reinterpret_cast<const char*>(current.data()),
                         static_cast<std::streamsize>(current.size() * sizeof(uint16_t)));
            const size_t reachable = static_cast<size_t>(std::count_if(
                current.begin(), current.end(), [](uint16_t value) { return value != kInf; }));
            std::cerr << "period=" << period << " reachable=" << reachable << '\n';
            if (period != max_periods)
                current = compose(current, base.costs, base.dimension, operations);
        }
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        std::cerr << "dimension=" << base.dimension << " periods=" << max_periods
                  << " operations=" << operations << " seconds=" << seconds << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
