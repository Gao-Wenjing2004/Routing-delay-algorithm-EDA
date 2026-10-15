#include "runtime_atlas_builder.hpp"
#include "srb_hybrid.hpp"
#include <chrono>
#include <array>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using srb_fast::Slice;

static void trim(Slice& s) {
  while (s.size && (s.data[0] == ' ' || s.data[0] == '\t' || s.data[0] == '\r')) { ++s.data; --s.size; }
  while (s.size && (s.data[s.size - 1] == ' ' || s.data[s.size - 1] == '\t' || s.data[s.size - 1] == '\r')) --s.size;
}
static bool fields(Slice line, Slice& a, Slice& b) {
  const char* c = static_cast<const char*>(std::memchr(line.data, ',', line.size));
  if (!c) return false;
  a = {line.data, size_t(c - line.data)};
  b = {c + 1, line.size - a.size - 1};
  trim(a); trim(b);
  return a.size && b.size;
}
static bool ci(Slice s, const char* t) {
  if (s.size != std::strlen(t)) return false;
  for (size_t i = 0; i < s.size; ++i) if ((s.data[i] >= 'A' && s.data[i] <= 'Z' ? s.data[i] + 32 : s.data[i]) != t[i]) return false;
  return true;
}

class UnifiedEstimator {
 public:
  UnifiedEstimator() : path_(temporary_path()) {
#if defined(SRB_SHARED_RUNTIME_ATLAS) && !defined(_WIN32)
    const std::string lock_path = path_ + ".lock";
    int lock_fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd < 0 || ::flock(lock_fd, LOCK_EX) != 0) {
      if (lock_fd >= 0) ::close(lock_fd);
      throw std::runtime_error("cannot lock shared runtime Atlas");
    }
    struct stat st{};
    if (::stat(path_.c_str(), &st) != 0 || uint64_t(st.st_size) != 1865748174ull) {
      const std::string partial = path_ + "." + std::to_string(::getpid()) + ".partial";
      std::remove(partial.c_str());
      srb_runtime_atlas::build(partial);
      if (::rename(partial.c_str(), path_.c_str()) != 0) {
        std::remove(partial.c_str());
        ::flock(lock_fd, LOCK_UN); ::close(lock_fd);
        throw std::runtime_error("cannot publish shared runtime Atlas");
      }
    }
    model_.reset(new srb_v3::HybridEstimator(path_, 72, 32, -1));
    ::flock(lock_fd, LOCK_UN); ::close(lock_fd);
#else
    srb_runtime_atlas::build(path_);
    model_.reset(new srb_v3::HybridEstimator(path_, 72, 32, -1));
#ifndef _WIN32
    std::remove(path_.c_str());
#endif
#endif
  }
  ~UnifiedEstimator() {
    model_.reset();
#ifndef SRB_SHARED_RUNTIME_ATLAS
    std::remove(path_.c_str());
#endif
  }
  bool predict(Slice line, uint32_t& delay, bool& exact) const {
    Slice from, to;
    if (!fields(line, from, to)) return false;
    return model_->predict_spec(from, to, delay, exact);
  }
 private:
  static std::string temporary_path() {
#ifdef _WIN32
    return "srb_runtime_atlas.tmp";
#else
#ifdef SRB_SHARED_RUNTIME_ATLAS
    return "/tmp/srb_runtime_atlas_v20.bin";
#else
    return std::string("/tmp/srb_runtime_atlas_") + std::to_string(::getpid()) + ".bin";
#endif
#endif
  }
  std::string path_;
  std::unique_ptr<srb_v3::HybridEstimator> model_;
};
static void number(std::string& o, uint32_t v) {
  char d[16], *e = d + sizeof(d), *p = e;
  do { *--p = char('0' + v % 10); v /= 10; } while (v);
  o.append(p, size_t(e - p));
}

struct NumberTable {
  std::array<std::array<char, 5>, 65536> text{};
  std::array<uint8_t, 65536> length{};
  NumberTable() {
    for (uint32_t v = 0; v < 65536; ++v) {
      char d[5], *e = d + 5, *p = e;
      uint32_t x = v;
      do { *--p = char('0' + x % 10); x /= 10; } while (x);
      length[v] = uint8_t(e - p);
      std::memcpy(text[v].data(), p, length[v]);
    }
  }
  void append(std::string& o, uint32_t v) const {
    if (v < 65536) o.append(text[v].data(), length[v]);
    else number(o, v);
  }
  void write(char*& out, uint32_t v) const {
    if (v < 65536) {
      std::memcpy(out, text[v].data(), length[v]);
      out += length[v];
    } else {
      char d[16], *e = d + 16, *p = e;
      do { *--p = char('0' + v % 10); v /= 10; } while (v);
      std::memcpy(out, p, size_t(e - p));
      out += e - p;
    }
  }
};

// Use the same bounded-memory sequential I/O path on every platform.  It does
// not depend on /proc/self/exe, mmap, ftruncate, sparse files, or cached rows.
#if 1
int main(int argc, char** argv) {
  const auto start = std::chrono::steady_clock::now();
  try {
    std::string in, out;
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "-in" && i + 1 < argc) in = argv[++i];
      else if (a == "-out" && i + 1 < argc) out = argv[++i];
      else throw std::runtime_error("usage: estimate -in input.csv -out output.csv");
    }
    if (in.empty() || out.empty() || in == out) throw std::runtime_error("invalid input/output path");
    using F = std::unique_ptr<FILE, decltype(&std::fclose)>;
    F fi(std::fopen(in.c_str(), "rb"), &std::fclose);
    F fo(std::fopen(out.c_str(), "wb"), &std::fclose);
    if (!fi) throw std::runtime_error("cannot open input");
    if (!fo) throw std::runtime_error("cannot open output");
    std::setvbuf(fi.get(), nullptr, _IOFBF, 1 << 20);
    std::setvbuf(fo.get(), nullptr, _IOFBF, 1 << 20);
    UnifiedEstimator est;
    const NumberTable numbers;
    // The official scorer requires the exact, case-sensitive header below.
    std::fwrite("From,To,Delay\n", 1, 14, fo.get());
    constexpr size_t B = 32u << 20;
    std::vector<char> buf(B + 4096);
    std::string output;
    output.reserve(B + (B >> 2));
    size_t carry = 0;
    bool first = true;
    uint64_t ok = 0, bad = 0, exact = 0;
    for (;;) {
      size_t n = std::fread(buf.data() + carry, 1, B, fi.get());
      bool eof = n < B;
      size_t total = carry + n, pos = 0;
      output.clear();
      while (pos < total) {
        const char* e = static_cast<const char*>(std::memchr(buf.data() + pos, '\n', total - pos));
        if (!e && !eof) break;
        size_t stop = e ? size_t(e - buf.data()) : total;
        Slice line{buf.data() + pos, stop - pos};
        pos = e ? stop + 1 : total;
        if (!line.size) continue;
        Slice a, b;
        if (first) {
          first = false;
          if (line.size >= 3 && !std::memcmp(line.data, "\xef\xbb\xbf", 3)) { line.data += 3; line.size -= 3; }
          if (fields(line, a, b) && ci(a, "from") && ci(b, "to")) continue;
        }
        uint32_t delay;
        bool hit;
        if (!est.predict(line, delay, hit)) { ++bad; continue; }
        exact += hit;
        size_t raw_size = line.size;
        while (raw_size && line.data[raw_size - 1] == '\r') --raw_size;
        output.append(line.data, raw_size); output.push_back(',');
        numbers.append(output, delay); output.push_back('\n');
        ++ok;
      }
      if (!output.empty() && std::fwrite(output.data(), 1, output.size(), fo.get()) != output.size()) throw std::runtime_error("output write failed");
      carry = total - pos;
      if (carry) std::memmove(buf.data(), buf.data() + pos, carry);
      if (eof) break;
    }
    if (std::fflush(fo.get())) throw std::runtime_error("output flush failed");
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cerr << "processed=" << ok << " bad_rows=" << bad << " exact_hits=" << exact
              << " fallback=" << (ok - exact) << " elapsed=" << sec
              << " throughput=" << (sec ? ok / sec : 0) << " q/s\n";
    return bad ? 1 : 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
  }
}
#else
int main(int argc, char** argv) {
  const auto start = std::chrono::steady_clock::now();
  int input_fd = -1, output_fd = -1;
  const char* input_map = static_cast<const char*>(MAP_FAILED);
  char* output_map = static_cast<char*>(MAP_FAILED);
  size_t input_size = 0, output_capacity = 0;
  try {
    std::string in, out;
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "-in" && i + 1 < argc) in = argv[++i];
      else if (a == "-out" && i + 1 < argc) out = argv[++i];
      else throw std::runtime_error("usage: estimate -in input.csv -out output.csv");
    }
    if (in.empty() || out.empty() || in == out) throw std::runtime_error("invalid input/output path");
    input_fd = ::open(in.c_str(), O_RDONLY);
    if (input_fd < 0) throw std::runtime_error("cannot open input");
    struct stat st{};
    if (fstat(input_fd, &st) != 0 || st.st_size <= 0) throw std::runtime_error("cannot stat input");
    input_size = size_t(st.st_size);
    input_map = static_cast<const char*>(mmap(nullptr, input_size, PROT_READ, MAP_PRIVATE, input_fd, 0));
    if (input_map == MAP_FAILED) throw std::runtime_error("cannot mmap input");
    madvise(const_cast<char*>(input_map), input_size, MADV_SEQUENTIAL);
    output_fd = ::open(out.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (output_fd < 0) throw std::runtime_error("cannot open output");
    // Public data expands from about 35.4 bytes/request to 40.3 bytes/result.
    // Reserving 2.5x the multi-billion-row input exceeded the judge's file
    // size/quota limit before the estimator was even constructed.  A 25%
    // margin covers the input line plus comma, delay digits and output header.
    output_capacity = input_size + input_size / 4 + (64u << 20);
    if (ftruncate(output_fd, off_t(output_capacity)) != 0) throw std::runtime_error("cannot size output");
    output_map = static_cast<char*>(mmap(nullptr, output_capacity, PROT_READ | PROT_WRITE, MAP_SHARED, output_fd, 0));
    if (output_map == MAP_FAILED) throw std::runtime_error("cannot mmap output");
    madvise(output_map, output_capacity, MADV_SEQUENTIAL);

    UnifiedEstimator est;
    const NumberTable numbers;
    char* dst = output_map;
    const char header[] = "From,To,Delay\n";
    std::memcpy(dst, header, sizeof(header) - 1);
    dst += sizeof(header) - 1;
    const char* p = input_map;
    const char* end = input_map + input_size;
    const size_t page_size = size_t(::sysconf(_SC_PAGESIZE));
    const size_t release_stride = 64u << 20;
    size_t input_released = 0;
    size_t output_released = 0;
    const char* first_nl = static_cast<const char*>(std::memchr(p, '\n', size_t(end - p)));
    if (!first_nl) throw std::runtime_error("missing input header");
    p = first_nl + 1;
    uint64_t ok = 0, bad = 0, hits = 0;
    uint64_t next_report = 100000000;
    while (p < end) {
      const char* nl = static_cast<const char*>(std::memchr(p, '\n', size_t(end - p)));
      const char* stop = nl ? nl : end;
      size_t raw_size = size_t(stop - p);
      while (raw_size && p[raw_size - 1] == '\r') --raw_size;
      if (raw_size) {
        Slice line{p, raw_size};
        uint32_t delay;
        bool hit;
        if (!est.predict(line, delay, hit)) {
          ++bad;
        } else {
          if (size_t(output_map + output_capacity - dst) < raw_size + 16) throw std::runtime_error("output mapping exhausted");
          std::memcpy(dst, p, raw_size); dst += raw_size;
          *dst++ = ',';
          numbers.write(dst, delay);
          *dst++ = '\n';
          hits += hit;
          ++ok;
          if (ok == next_report) {
            const double now = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start).count();
            std::cerr << "progress=" << ok << " cache_hits=" << hits
                      << " fallback=" << (ok - hits)
                      << " elapsed=" << now << '\n';
            next_report += 100000000;
          }
        }
      }
      p = nl ? nl + 1 : end;
      const size_t input_used = size_t(p - input_map);
      const size_t output_used = size_t(dst - output_map);
      if (input_used - input_released >= release_stride) {
        const size_t upto = input_used & ~(page_size - 1);
        if (upto > input_released) {
          madvise(const_cast<char*>(input_map) + input_released,
                  upto - input_released, MADV_DONTNEED);
          input_released = upto;
        }
      }
      if (output_used - output_released >= release_stride) {
        const size_t upto = output_used & ~(page_size - 1);
        if (upto > output_released) {
          // Queue dirty pages for writeback, then let the kernel reclaim them.
          // This keeps a tens-of-gigabytes result file from becoming resident.
          if (msync(output_map + output_released, upto - output_released,
                    MS_ASYNC) != 0)
            throw std::runtime_error("cannot schedule output flush");
          madvise(output_map + output_released, upto - output_released,
                  MADV_DONTNEED);
          output_released = upto;
        }
      }
    }
    size_t output_size = size_t(dst - output_map);
    if (msync(output_map, output_size, MS_ASYNC) != 0) throw std::runtime_error("cannot schedule output flush");
    if (munmap(output_map, output_capacity) != 0) throw std::runtime_error("cannot unmap output");
    output_map = static_cast<char*>(MAP_FAILED);
    if (ftruncate(output_fd, off_t(output_size)) != 0) throw std::runtime_error("cannot finalize output");
    ::close(output_fd); output_fd = -1;
    munmap(const_cast<char*>(input_map), input_size);
    input_map = static_cast<const char*>(MAP_FAILED);
    ::close(input_fd); input_fd = -1;
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cerr << "processed=" << ok << " bad_rows=" << bad << " cache_hits=" << hits
              << " elapsed=" << sec << " throughput=" << (sec ? ok / sec : 0) << " q/s\n";
    return bad ? 1 : 0;
  } catch (const std::exception& e) {
    if (output_map != MAP_FAILED) munmap(output_map, output_capacity);
    if (input_map != MAP_FAILED) munmap(const_cast<char*>(input_map), input_size);
    if (output_fd >= 0) ::close(output_fd);
    if (input_fd >= 0) ::close(input_fd);
    std::cerr << "error: " << e.what() << '\n';
    return 1;
  }
}
#endif
