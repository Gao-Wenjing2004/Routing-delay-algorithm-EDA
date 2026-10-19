#pragma once
#include <cstdint>
namespace e2e {
struct Header {
    char magic[8]; uint32_t version, ports, base_radius, hot_radius;
    uint32_t sources, targets, hot_sources, hot_targets, hot_cells, reserved;
    uint64_t base_values, hot_values;
};
static_assert(sizeof(Header)==64,"E2E header layout");
}
