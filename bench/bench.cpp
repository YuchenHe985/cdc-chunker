// cdc_bench: throughput, chunk size distribution and deduplication after edits. Prints markdown.
//
//   cdc_bench throughput [--size-mb 256] [--reps 7]
//   cdc_bench sizes      [--size-mb 64]
//   cdc_bench dedup      --corpus DIR [--cap-mb 64] [--edits 200] [--seed 1]
//   cdc_bench all        --corpus DIR
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "cdc/cdc.hpp"

namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

struct Args {
  std::string mode;
  std::string corpus;
  std::size_t size_mb = 256;
  int reps = 7;
  std::size_t cap_mb = 64;
  int edits = 200;
  std::uint64_t seed = 1;
};

Args parse(int argc, char** argv) {
  if (argc < 2) throw std::invalid_argument("mode required: throughput, sizes, dedup or all");
  Args a;
  a.mode = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string f = argv[i];
    if (i + 1 >= argc) throw std::invalid_argument(f + " needs a value");
    const char* v = argv[++i];
    if (f == "--corpus") a.corpus = v;
    else if (f == "--size-mb") a.size_mb = static_cast<std::size_t>(std::atoll(v));
    else if (f == "--reps") a.reps = std::atoi(v);
    else if (f == "--cap-mb") a.cap_mb = static_cast<std::size_t>(std::atoll(v));
    else if (f == "--edits") a.edits = std::atoi(v);
    else if (f == "--seed") a.seed = static_cast<std::uint64_t>(std::atoll(v));
    else throw std::invalid_argument("unknown option " + f);
  }
  return a;
}

Bytes random_bytes(std::size_t n, std::uint64_t seed) {
  Bytes out(n);
  std::uint64_t state = seed;
  for (std::size_t i = 0; i < n; i += 8) {
    const std::uint64_t v = cdc::detail::splitmix64(state);
    for (std::size_t j = 0; j < 8 && i + j < n; ++j) out[i + j] = static_cast<std::uint8_t>(v >> (8 * j));
  }
  return out;
}

struct Spec {
  std::string label;
  std::string algo;
  cdc::Params params;
};

std::vector<Spec> specs(std::size_t avg) {
  cdc::Params p = cdc::Params::from_average(avg);
  cdc::Params flat = p;
  flat.normalization = 0;
  return {{"fixed", "fixed", p}, {"rabin", "rabin", p}, {"gear (norm 0)", "gear", flat}, {"gear (norm 2)", "gear", p}};
}

double seconds_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

std::size_t count_chunks(cdc::Chunker& c, const Bytes& d, std::size_t* sum) {
  std::size_t n = 0, total = 0;
  const cdc::Sink sink = [&](std::uint64_t, std::size_t len) {
    ++n;
    total += len;
  };
  c.reset();
  c.feed(d.data(), d.size(), sink);
  c.finish(sink);
  *sum = total;
  return n;
}

void throughput(const Args& a) {
  const Bytes data = random_bytes(a.size_mb << 20, 7);
  const double mb = static_cast<double>(data.size()) / 1e6;
  std::printf("## Throughput\n\nSingle thread, %zu MiB of pseudo-random bytes, median of %d runs after one warm-up.\n\n",
              a.size_mb, a.reps);
  std::printf("| chunker | avg size | median MB/s | min | max | chunks |\n| --- | ---: | ---: | ---: | ---: | ---: |\n");
  for (std::size_t avg : {std::size_t{2048}, std::size_t{8192}, std::size_t{32768}}) {
    for (const Spec& s : specs(avg)) {
      if (s.algo == "fixed") continue;  // never reads the data, so its speed says nothing
      const auto chunker = cdc::make_chunker(s.algo, s.params);
      std::size_t sum = 0, chunks = 0;
      chunks = count_chunks(*chunker, data, &sum);  // warm-up
      std::vector<double> rate;
      for (int r = 0; r < a.reps; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        chunks = count_chunks(*chunker, data, &sum);
        rate.push_back(mb / seconds_since(t0));
      }
      if (sum != data.size()) throw std::runtime_error("chunks do not cover the buffer");
      std::sort(rate.begin(), rate.end());
      std::printf("| %s | %zu | %.0f | %.0f | %.0f | %zu |\n", s.label.c_str(), avg, rate[rate.size() / 2], rate.front(),
                  rate.back(), chunks);
    }
  }
  // Reference: what fingerprinting every chunk costs, and the plain memory-scan speed.
  std::vector<double> rate;
  volatile std::uint64_t sink = 0;
  for (int r = 0; r < a.reps; ++r) {
    const auto t0 = std::chrono::steady_clock::now();
    sink = cdc::detail::fnv1a64(data.data(), data.size());
    rate.push_back(mb / seconds_since(t0));
  }
  (void)sink;
  std::sort(rate.begin(), rate.end());
  std::printf("| FNV-1a 64 over the buffer (reference) | - | %.0f | %.0f | %.0f | - |\n\n", rate[rate.size() / 2],
              rate.front(), rate.back());
}

struct SizeStats {
  std::size_t chunks = 0;
  double mean = 0, cv = 0, p5 = 0, p50 = 0, p95 = 0, forced_pct = 0;
};

SizeStats size_stats(const Spec& s, const Bytes& d) {
  const auto chunker = cdc::make_chunker(s.algo, s.params);
  const auto chunks = cdc::chunk_all(*chunker, d.data(), d.size());
  std::vector<double> len;
  std::size_t forced = 0;
  for (std::size_t i = 0; i + 1 < chunks.size(); ++i) {  // the last chunk is cut by the end of input
    len.push_back(static_cast<double>(chunks[i].length));
    if (chunks[i].length == s.params.max_size) ++forced;
  }
  SizeStats st;
  st.chunks = chunks.size();
  double sum = 0, sq = 0;
  for (double l : len) {
    sum += l;
    sq += l * l;
  }
  const double n = static_cast<double>(len.size());
  st.mean = sum / n;
  st.cv = std::sqrt(sq / n - st.mean * st.mean) / st.mean;
  std::sort(len.begin(), len.end());
  auto pct = [&](double q) { return len[static_cast<std::size_t>(q * (n - 1))]; };
  st.p5 = pct(0.05);
  st.p50 = pct(0.5);
  st.p95 = pct(0.95);
  st.forced_pct = 100.0 * static_cast<double>(forced) / n;
  return st;
}

void sizes(const Args& a) {
  const Bytes data = random_bytes(std::min<std::size_t>(a.size_mb, 64) << 20, 9);
  std::printf("## Chunk size distribution\n\n%zu MiB of pseudo-random bytes; min = avg/4, max = avg*8. "
              "cv is stddev / mean.\n\n",
              data.size() >> 20);
  std::printf("| chunker | target avg | mean | cv | p5 | p50 | p95 | cut at max |\n| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n");
  for (std::size_t avg : {std::size_t{2048}, std::size_t{8192}, std::size_t{32768}})
    for (const Spec& s : specs(avg)) {
      if (s.algo == "fixed") continue;
      const SizeStats st = size_stats(s, data);
      std::printf("| %s | %zu | %.0f | %.2f | %.0f | %.0f | %.0f | %.2f%% |\n", s.label.c_str(), avg, st.mean, st.cv, st.p5,
                  st.p50, st.p95, st.forced_pct);
    }
  std::printf("\n");
}

Bytes load_corpus(const std::string& dir, std::size_t cap, std::size_t* files) {
  std::vector<fs::path> paths;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; it != end;
       it.increment(ec)) {
    if (ec) break;
    std::error_code e2;
    if (it->is_regular_file(e2) && !e2) paths.push_back(it->path());
  }
  std::sort(paths.begin(), paths.end());
  Bytes out;
  *files = 0;
  for (const fs::path& p : paths) {
    std::ifstream in(p, std::ios::binary);
    if (!in) continue;
    Bytes buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (buf.empty()) continue;
    out.insert(out.end(), buf.begin(), buf.end());
    ++*files;
    if (out.size() >= cap) break;
  }
  if (out.size() > cap) out.resize(cap);
  return out;
}

// A later version of `base`: `edits` random insertions, deletions and overwrites of 1-64 bytes.
Bytes edit(const Bytes& base, int edits, std::uint64_t seed) {
  std::uint64_t st = seed;
  struct Edit {
    std::size_t pos;
    int kind;
    std::size_t len;
  };
  std::vector<Edit> list;
  for (int i = 0; i < edits; ++i)
    list.push_back({static_cast<std::size_t>(cdc::detail::splitmix64(st) % base.size()),
                    static_cast<int>(cdc::detail::splitmix64(st) % 3), 1 + cdc::detail::splitmix64(st) % 64});
  std::sort(list.begin(), list.end(), [](const Edit& a, const Edit& b) { return a.pos < b.pos; });
  Bytes out;
  out.reserve(base.size() + static_cast<std::size_t>(edits) * 64);
  std::size_t cursor = 0;
  for (const Edit& e : list) {
    if (e.pos < cursor) continue;
    out.insert(out.end(), base.begin() + static_cast<std::ptrdiff_t>(cursor), base.begin() + static_cast<std::ptrdiff_t>(e.pos));
    cursor = e.pos;
    const Bytes fresh = random_bytes(e.len, cdc::detail::splitmix64(st));
    if (e.kind == 0) {  // insert
      out.insert(out.end(), fresh.begin(), fresh.end());
    } else if (e.kind == 1) {  // delete
      cursor = std::min(base.size(), cursor + e.len);
    } else {  // overwrite
      out.insert(out.end(), fresh.begin(), fresh.end());
      cursor = std::min(base.size(), cursor + e.len);
    }
  }
  out.insert(out.end(), base.begin() + static_cast<std::ptrdiff_t>(cursor), base.end());
  return out;
}

void dedup(const Args& a) {
  if (a.corpus.empty()) throw std::invalid_argument("dedup needs --corpus DIR");
  std::size_t files = 0;
  const Bytes v1 = load_corpus(a.corpus, a.cap_mb << 20, &files);
  if (v1.empty()) throw std::runtime_error("corpus is empty");
  const Bytes v2 = edit(v1, a.edits, a.seed);
  std::printf("## Deduplication after edits\n\nVersion 1: %zu files, %.1f MB (concatenated in path order). Version 2: %d random "
              "insertions, deletions and overwrites of 1-64 bytes (seed %llu), %.1f MB.\n"
              "\"Shared\" is the fraction of version 2 that lies in chunks byte-identical to a chunk of version 1 "
              "(64-bit FNV-1a identity).\n\n",
              files, static_cast<double>(v1.size()) / 1e6, a.edits, static_cast<unsigned long long>(a.seed),
              static_cast<double>(v2.size()) / 1e6);
  std::printf("| chunker | avg size | chunks (v2) | mean chunk | shared with v1 | new bytes to store | per edit |\n| --- | ---: | ---: | ---: | ---: | ---: | ---: |\n");
  for (std::size_t avg : {std::size_t{2048}, std::size_t{8192}, std::size_t{32768}})
    for (const Spec& s : specs(avg)) {
      const auto chunker = cdc::make_chunker(s.algo, s.params);
      std::unordered_set<std::uint64_t> known;
      for (const auto& c : cdc::chunk_all(*chunker, v1.data(), v1.size()))
        known.insert(cdc::detail::fnv1a64(v1.data() + c.offset, c.length));
      const auto chunks = cdc::chunk_all(*chunker, v2.data(), v2.size());
      std::size_t shared = 0;
      for (const auto& c : chunks)
        if (known.count(cdc::detail::fnv1a64(v2.data() + c.offset, c.length)) != 0) shared += c.length;
      const double fresh = static_cast<double>(v2.size() - shared);
      std::printf("| %s | %zu | %zu | %.0f | %.2f%% | %.2f MB | %.1f KB |\n", s.label.c_str(), avg, chunks.size(),
                  static_cast<double>(v2.size()) / static_cast<double>(chunks.size()),
                  100.0 * static_cast<double>(shared) / static_cast<double>(v2.size()), fresh / 1e6,
                  fresh / 1e3 / static_cast<double>(a.edits));
    }
  std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Args a = parse(argc, argv);
    if (a.mode == "throughput" || a.mode == "all") throughput(a);
    if (a.mode == "sizes" || a.mode == "all") sizes(a);
    if (a.mode == "dedup" || a.mode == "all") dedup(a);
    if (a.mode != "throughput" && a.mode != "sizes" && a.mode != "dedup" && a.mode != "all")
      throw std::invalid_argument("unknown mode " + a.mode);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "cdc_bench: %s\n", e.what());
    return 1;
  }
}
