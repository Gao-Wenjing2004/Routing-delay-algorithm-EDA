#pragma once
#include <array>
#include <cstdint>

namespace srb_v11_normalized_hull_binding {
struct Section {
    uint64_t offset, bytes, raw_bytes;
    uint32_t compressed_crc, raw_crc;
    const char* compressed_sha;
    const char* raw_sha;
};
struct Binding {
    uint64_t asset_bytes, header_bytes;
    uint32_t json_crc, dictionary_entries, uid_width;
    const char* asset_sha;
    std::array<Section,7> sections;
};
// asset_binding.hpp is generated ONLY in the independently Root-released BUILD.
// Its exact JSON CRC, section sizes/CRC/SHA and trusted whole SHA are frozen from
// the completed full110 asset, after independently proving the source reports.
}
