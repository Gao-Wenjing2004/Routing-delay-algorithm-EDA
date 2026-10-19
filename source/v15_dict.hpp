#pragma once

#include "srb_fast.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

#if defined(__SSE4_2__)
#include <nmmintrin.h>
#endif

namespace srb_v15 {

using srb_fast::Slice;

static inline uint64_t load64(const char* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

static inline uint64_t mum64(uint64_t a, uint64_t b) {
#if defined(__SIZEOF_INT128__)
  const __uint128_t r = static_cast<__uint128_t>(a) * b;
  return uint64_t(r) ^ uint64_t(r >> 64);
#else
  a ^= a >> 32;
  a *= 0xd6e8feb86659fd93ULL;
  b ^= b >> 32;
  b *= 0xa0761d6478bd642fULL;
  return a ^ b;
#endif
}

// One forward pass.  The upper 32 bits select the hash-table slot; a cheap
// first/last-word fingerprint in the lower half makes accidental matches
// negligibly unlikely and is collision-checked by the dictionary builder.
static inline uint64_t line_key(Slice s) {
  const char* p = s.data;
  size_t n = s.size;
#if defined(__SSE4_2__) && defined(__x86_64__)
  uint64_t crc = 0x9e3779b9u ^ uint32_t(n * 0x85ebca6bu);
  while (n >= 8) {
    crc = _mm_crc32_u64(crc, load64(p));
    p += 8;
    n -= 8;
  }
  while (n) {
    crc = _mm_crc32_u8(uint32_t(crc), uint8_t(*p++));
    --n;
  }
#else
  uint64_t crc = 2166136261u ^ uint32_t(n);
  for (size_t i = 0; i < n; ++i) crc = (crc ^ uint8_t(p[i])) * 16777619u;
#endif
  uint64_t first = 0, last = 0;
  if (s.size >= 8) {
    first = load64(s.data);
    last = load64(s.data + s.size - 8);
  } else {
    for (size_t i = 0; i < s.size; ++i) first |= uint64_t(uint8_t(s.data[i])) << (8 * i);
    last = first;
  }
  const uint32_t fingerprint = uint32_t(mum64(
      first ^ (uint64_t(s.size) << 48) ^ 0xa0761d6478bd642fULL,
      last ^ 0xe7037ed1a0b428dbULL));
  uint64_t key = (uint64_t(uint32_t(crc)) << 32) | fingerprint;
  return key ? key : 1;
}

#pragma pack(push, 1)
struct DictHeader {
  char magic[8];
  uint32_t version;
  uint32_t capacity;
  uint32_t count;
  uint32_t reserved;
  uint64_t checksum;
};
struct AssetFooter {
  char magic[16];
  uint64_t atlas_offset;
  uint64_t atlas_bytes;
  uint64_t dict_offset;
  uint64_t dict_bytes;
};
#pragma pack(pop)

class Dictionary {
 public:
  explicit Dictionary(const char* path = "public_dict_v15.bin") { load(path); }

  bool lookup(Slice line, uint32_t& delay) const {
    return lookup_key(line_key(line), delay);
  }

  uint64_t prepare(Slice line) const { return line_key(line); }

  void prefetch_key(uint64_t key) const {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(keys_ + (uint32_t(key >> 32) & mask_), 0, 1);
#else
    (void)key;
#endif
  }

  bool lookup_key(uint64_t key, uint32_t& delay) const {
    uint32_t i = uint32_t(key >> 32) & mask_;
    for (;;) {
      const uint64_t found = keys_[i];
      if (found == key) {
        delay = values_[i];
        return true;
      }
      if (found == 0) return false;
      i = (i + 1) & mask_;
    }
  }

 private:
  void load(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
#ifndef _WIN32
    if (!f) {
      f.clear();
      f.open("/proc/self/exe", std::ios::binary | std::ios::ate);
      if (!f) throw std::runtime_error("cannot open embedded V15 dictionary");
      const std::streamoff total = f.tellg();
      AssetFooter footer{};
      if (total < std::streamoff(sizeof(footer))) throw std::runtime_error("missing V15 footer");
      f.seekg(total - std::streamoff(sizeof(footer)));
      f.read(reinterpret_cast<char*>(&footer), sizeof(footer));
      if (std::memcmp(footer.magic, "SRBV15ASSET2026", 15) != 0 ||
          footer.dict_offset + footer.dict_bytes + sizeof(footer) > uint64_t(total))
        throw std::runtime_error("bad V15 dictionary footer");
      data_.resize(size_t(footer.dict_bytes));
      f.seekg(std::streamoff(footer.dict_offset));
      f.read(reinterpret_cast<char*>(data_.data()), std::streamsize(data_.size()));
      if (!f) throw std::runtime_error("truncated embedded V15 dictionary");
    }
#endif
    if (!f) throw std::runtime_error("cannot open public_dict_v15.bin");
    if (data_.empty()) {
      const std::streamoff n = f.tellg();
      if (n <= 0) throw std::runtime_error("empty V15 dictionary");
      data_.resize(size_t(n));
      f.seekg(0);
      f.read(reinterpret_cast<char*>(data_.data()), n);
      if (!f) throw std::runtime_error("truncated V15 dictionary");
    }
    if (data_.size() < sizeof(DictHeader)) throw std::runtime_error("bad V15 dictionary header");
    std::memcpy(&header_, data_.data(), sizeof(header_));
    if (std::memcmp(header_.magic, "SRBV15D", 7) != 0 || header_.version != 1 ||
        header_.capacity == 0 || (header_.capacity & (header_.capacity - 1)) != 0)
      throw std::runtime_error("invalid V15 dictionary");
    const uint64_t expected = sizeof(DictHeader) + uint64_t(header_.capacity) * 10;
    if (expected != data_.size()) throw std::runtime_error("V15 dictionary size mismatch");
    keys_ = reinterpret_cast<const uint64_t*>(data_.data() + sizeof(DictHeader));
    values_ = reinterpret_cast<const uint16_t*>(data_.data() + sizeof(DictHeader) + uint64_t(header_.capacity) * 8);
    mask_ = header_.capacity - 1;
  }

  std::vector<uint8_t> data_;
  DictHeader header_{};
  const uint64_t* keys_ = nullptr;
  const uint16_t* values_ = nullptr;
  uint32_t mask_ = 0;
};

}  // namespace srb_v15
