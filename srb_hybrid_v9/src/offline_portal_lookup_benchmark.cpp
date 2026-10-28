#include "p10_periodic_portal.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

template <class T>
T read_value(std::istream& input) {
    T value{};
    if (!input.read(reinterpret_cast<char*>(&value), sizeof(value)))
        throw std::runtime_error("truncated packed Portal closure");
    return value;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3 && argc != 4)
            throw std::runtime_error(
                "usage: offline_portal_lookup_benchmark packed.bin iterations [raw_closure.bin]");
        std::ifstream packed_input(argv[1], std::ios::binary);
        if (!packed_input) throw std::runtime_error("cannot open packed Portal closure");
        packed_input.seekg(0, std::ios::end);
        const std::streamsize packed_size = packed_input.tellg();
        if (packed_size < 0) throw std::runtime_error("cannot size packed Portal closure");
        packed_input.seekg(0, std::ios::beg);
        std::vector<unsigned char> packed(static_cast<std::size_t>(packed_size));
        if (!packed_input.read(reinterpret_cast<char*>(packed.data()), packed_size))
            throw std::runtime_error("cannot read packed Portal closure");
        const p10::PackedPortalClosureView closure(packed.data(), packed.size());
        if (argc == 4) {
            std::ifstream raw(argv[3], std::ios::binary);
            char header[20];
            if (!raw.read(header, sizeof(header)) || std::memcmp(header, "PPCLOS01", 8) != 0)
                throw std::runtime_error("bad raw closure for verification");
            uint64_t mismatches = 0;
            for (int period = 1; period <= closure.periods(); ++period) {
                for (uint16_t source = 0; source < closure.dimension(); ++source) {
                    for (uint16_t target = 0; target < closure.dimension(); ++target) {
                        const uint16_t expected = read_value<uint16_t>(raw);
                        if (closure.lookup(period, source, target) != expected) ++mismatches;
                    }
                }
            }
            if (mismatches)
                throw std::runtime_error("packed Portal verification mismatches=" +
                                         std::to_string(mismatches));
            std::cout << "verified_entries="
                      << static_cast<uint64_t>(closure.periods()) * closure.dimension() *
                             closure.dimension()
                      << " mismatches=0\n";
        }
        const uint64_t iterations = std::stoull(argv[2]);
        uint32_t state = 0x9e3779b9u;
        uint64_t checksum = 0;
        const auto started = std::chrono::steady_clock::now();
        for (uint64_t index = 0; index < iterations; ++index) {
            state = state * 1664525u + 1013904223u;
            const uint16_t source = static_cast<uint16_t>(state % closure.dimension());
            state = state * 1664525u + 1013904223u;
            const uint16_t target = static_cast<uint16_t>(state % closure.dimension());
            const int period = 1 + static_cast<int>((state >> 24) % closure.periods());
            checksum += closure.lookup(period, source, target);
        }
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        std::cout << "iterations=" << iterations << " seconds=" << seconds
                  << " ns_per_lookup=" << (seconds * 1e9 / iterations)
                  << " checksum=" << checksum << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
