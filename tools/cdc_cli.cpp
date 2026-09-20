// cdc: command line front end for the chunking library.
//
//   cdc chunk [options] FILE     one line per chunk: offset, length, 64-bit FNV-1a hash
//   cdc stats [options] FILE     chunk count and size distribution
//   cdc dedup [options] OLD NEW  how much of NEW is already present in OLD as whole chunks
//   cdc info                     constants that define the chunk boundaries
//
// options: --algo gear|rabin|fixed  --avg N  --min N  --max N  --norm N
//          --threads N (0 = all cores; same chunks as one thread)  --segment-min N
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "cdc/cdc.hpp"

namespace {

struct Options {
  std::string algo = "gear";
  cdc::Params params;
  unsigned threads = 1;              // 0 = one per hardware thread
  std::size_t segment_min = 256 * 1024;
  std::vector<std::string> files;
};

std::size_t parse_size(const std::string& flag, const char* text) {
  char* end = nullptr;
  const unsigned long long v = std::strtoull(text, &end, 10);
  if (end == text || *end != '\0') throw std::invalid_argument(flag + " expects a number");
  return static_cast<std::size_t>(v);
}

Options parse(int argc, char** argv, int first) {
  Options o;
  std::size_t avg = 0, min_v = 0, max_v = 0;
  int norm = -1;
  unsigned threads = 1;
  std::size_t segment_min = 256 * 1024;
  for (int i = first; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&]() -> const char* {
      if (i + 1 >= argc) throw std::invalid_argument(a + " needs a value");
      return argv[++i];
    };
    if (a == "--algo") {
      o.algo = value();
    } else if (a == "--avg") {
      avg = parse_size(a, value());
    } else if (a == "--min") {
      min_v = parse_size(a, value());
    } else if (a == "--max") {
      max_v = parse_size(a, value());
    } else if (a == "--norm") {
      norm = static_cast<int>(parse_size(a, value()));
    } else if (a == "--threads") {
      threads = static_cast<unsigned>(parse_size(a, value()));
    } else if (a == "--segment-min") {
      segment_min = parse_size(a, value());
    } else if (!a.empty() && a[0] == '-') {
      throw std::invalid_argument("unknown option " + a);
    } else {
      o.files.push_back(a);
    }
  }
  // --avg sets min = avg / 4 and max = avg * 8; explicit --min, --max and --norm override that.
  if (avg != 0) o.params = cdc::Params::from_average(avg);
  if (min_v != 0) o.params.min_size = min_v;
  if (max_v != 0) o.params.max_size = max_v;
  if (norm >= 0) o.params.normalization = norm;
  o.threads = threads;
  o.segment_min = segment_min;
  return o;
}

std::vector<std::uint8_t> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + path);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<cdc::Chunk> chunk_file(const Options& o, const std::vector<std::uint8_t>& data) {
  if (o.threads != 1) return cdc::chunk_parallel(o.algo, o.params, data.data(), data.size(), o.threads, o.segment_min);
  const auto chunker = cdc::make_chunker(o.algo, o.params);
  return cdc::chunk_all(*chunker, data.data(), data.size());
}

std::uint64_t chunk_hash(const std::vector<std::uint8_t>& data, const cdc::Chunk& c) {
  return cdc::detail::fnv1a64(data.data() + c.offset, c.length);
}

int cmd_chunk(const Options& o) {
  if (o.files.size() != 1) throw std::invalid_argument("chunk expects one file");
  const auto data = read_file(o.files[0]);
  for (const cdc::Chunk& c : chunk_file(o, data))
    std::printf("%llu\t%zu\t%016llx\n", static_cast<unsigned long long>(c.offset), c.length,
                static_cast<unsigned long long>(chunk_hash(data, c)));
  return 0;
}

int cmd_stats(const Options& o) {
  if (o.files.size() != 1) throw std::invalid_argument("stats expects one file");
  const auto data = read_file(o.files[0]);
  const auto chunks = chunk_file(o, data);
  if (chunks.empty()) {
    std::puts("empty input");
    return 0;
  }
  std::vector<std::size_t> len;
  double sum = 0;
  std::size_t at_max = 0;
  for (const auto& c : chunks) {
    len.push_back(c.length);
    sum += static_cast<double>(c.length);
    if (c.length == o.params.max_size) ++at_max;
  }
  std::sort(len.begin(), len.end());
  const double mean = sum / static_cast<double>(len.size());
  double var = 0;
  for (std::size_t l : len) var += (static_cast<double>(l) - mean) * (static_cast<double>(l) - mean);
  const double sd = std::sqrt(var / static_cast<double>(len.size()));
  auto pct = [&](double q) { return len[static_cast<std::size_t>(q * static_cast<double>(len.size() - 1))]; };
  std::printf("algo=%s bytes=%zu chunks=%zu\n", o.algo.c_str(), data.size(), len.size());
  std::printf("mean=%.0f stddev=%.0f cv=%.2f min=%zu p5=%zu p50=%zu p95=%zu max=%zu forced_at_max=%zu\n", mean, sd,
              sd / mean, len.front(), pct(0.05), pct(0.5), pct(0.95), len.back(), at_max);
  return 0;
}

int cmd_dedup(const Options& o) {
  if (o.files.size() != 2) throw std::invalid_argument("dedup expects two files: OLD NEW");
  const auto old_data = read_file(o.files[0]);
  const auto new_data = read_file(o.files[1]);
  std::unordered_set<std::uint64_t> known;
  for (const auto& c : chunk_file(o, old_data)) known.insert(chunk_hash(old_data, c));
  std::size_t shared = 0, total = 0, chunks = 0, shared_chunks = 0;
  for (const auto& c : chunk_file(o, new_data)) {
    ++chunks;
    total += c.length;
    if (known.count(chunk_hash(new_data, c)) != 0) {
      shared += c.length;
      ++shared_chunks;
    }
  }
  std::printf("algo=%s new_bytes=%zu chunks=%zu shared_chunks=%zu shared_bytes=%zu shared=%.1f%%\n", o.algo.c_str(),
              total, chunks, shared_chunks, shared,
              total == 0 ? 0.0 : 100.0 * static_cast<double>(shared) / static_cast<double>(total));
  return 0;
}

int cmd_info() {
  std::printf("rabin_poly=0x%llX\n", static_cast<unsigned long long>(cdc::detail::kRabinPoly));
  std::printf("rabin_window=%zu\n", cdc::detail::kRabinWindow);
  std::printf("gear_seed=0x%llX\n", static_cast<unsigned long long>(cdc::detail::kGearSeed));
  std::printf("gear_window=%zu\n", cdc::detail::kGearWindow);
  return 0;
}

void usage() {
  std::fputs(
      "usage: cdc chunk|stats [options] FILE\n"
      "       cdc dedup [options] OLD NEW\n"
      "       cdc info\n"
      "options: --algo gear|rabin|fixed  --avg N  --min N  --max N  --norm N\n"
      "         --threads N (0 = all cores; same chunks as the default single thread)  --segment-min N\n",
      stderr);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string cmd = argv[1];
  try {
    if (cmd == "info") return cmd_info();
    if (cmd == "chunk" || cmd == "stats" || cmd == "dedup") {
      const Options o = parse(argc, argv, 2);
      if (cmd == "chunk") return cmd_chunk(o);
      if (cmd == "stats") return cmd_stats(o);
      return cmd_dedup(o);
    }
    usage();
    return 2;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "cdc: %s\n", e.what());
    return 1;
  }
}
