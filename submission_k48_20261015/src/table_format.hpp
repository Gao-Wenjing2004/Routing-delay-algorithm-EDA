#pragma once
#include <cstdint>
namespace srb_v8 {
struct Header {
    char magic[8];
    uint32_t version,ports,radius,sources,targets,cells,bits,record_bytes;
    uint64_t values,payload_bytes,reserved;
};
static_assert(sizeof(Header)==64,"table header");
struct CompressedHeader { char magic[8];uint32_t version,reserved;uint64_t raw_bytes,compressed_bytes; };
static_assert(sizeof(CompressedHeader)==32,"compressed header");
}
