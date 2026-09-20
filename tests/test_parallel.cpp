#include <algorithm>
#include <string>
#include <vector>

#include "minitest.hpp"

namespace {

bool same(const std::vector<cdc::Chunk>& a, const std::vector<cdc::Chunk>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (a[i].offset != b[i].offset || a[i].length != b[i].length) return false;
  return true;
}

std::vector<cdc::Chunk> sequential(const std::string& algo, const cdc::Params& p, const std::vector<std::uint8_t>& d) {
  const auto chunker = cdc::make_chunker(algo, p);
  return cdc::chunk_all(*chunker, d.data(), d.size());
}

std::vector<std::uint8_t> content(std::uint64_t kind, std::size_t n, std::uint64_t seed) {
  if (kind == 0) return minitest::random_bytes(n, seed);
  if (kind == 1) return std::vector<std::uint8_t>(n, static_cast<std::uint8_t>(seed));
  std::vector<std::uint8_t> out;
  const auto block = minitest::random_bytes(1 + seed % 3000, seed);
  for (std::size_t i = 0; i < n; ++i) out.push_back(block[i % block.size()]);
  return out;
}

}  // namespace

// The point of chunk_parallel: for any input, parameters, thread count and segment size the chunks are
// exactly those of the sequential chunker. Tiny segments (shorter than the hash window) are included on
// purpose, because that is where a wrong warm-up at a segment boundary would show.
TEST(parallel_chunks_equal_sequential_chunks) {
  std::uint64_t state = 424242;
  auto rnd = [&state](std::uint64_t n) { return cdc::detail::splitmix64(state) % n; };
  const std::size_t segments[] = {1, 7, 64, 1000, 5000, 262144};
  for (int iter = 0; iter < 500; ++iter) {
    const std::string algo = rnd(2) == 0 ? "gear" : "rabin";
    cdc::Params p;
    p.min_size = 64 + rnd(1500);
    p.avg_size = std::size_t{128} << rnd(7);
    while (p.avg_size <= p.min_size) p.avg_size <<= 1;
    p.max_size = p.avg_size * (2 + rnd(7));
    p.normalization = static_cast<int>(rnd(5));
    const std::size_t n = rnd(90000);
    const auto data = content(rnd(3), n, rnd(1000000));
    const unsigned threads = static_cast<unsigned>(1 + rnd(9));
    const std::size_t segment = segments[rnd(6)];
    const auto par = cdc::chunk_parallel(algo, p, data.data(), data.size(), threads, segment);
    if (!same(par, sequential(algo, p, data)))
      minitest::fail(__FILE__, __LINE__,
                     algo + " min=" + std::to_string(p.min_size) + " avg=" + std::to_string(p.avg_size) +
                         " max=" + std::to_string(p.max_size) + " norm=" + std::to_string(p.normalization) +
                         " n=" + std::to_string(n) + " threads=" + std::to_string(threads) +
                         " segment=" + std::to_string(segment));
  }
}

TEST(parallel_result_does_not_depend_on_the_thread_count) {
  const auto data = minitest::random_bytes(400000, 77);
  const cdc::Params p = cdc::Params::from_average(2048);
  for (const char* algo : {"gear", "rabin"}) {
    const auto expected = sequential(algo, p, data);
    for (unsigned threads = 1; threads <= 12; ++threads)
      if (!same(cdc::chunk_parallel(algo, p, data.data(), data.size(), threads, 1), expected))
        minitest::fail(__FILE__, __LINE__, std::string(algo) + " differs with " + std::to_string(threads) + " threads");
  }
}

TEST(parallel_handles_empty_and_window_sized_inputs) {
  const cdc::Params p = cdc::Params::from_average(128);
  CHECK(cdc::chunk_parallel("gear", p, nullptr, 0, 8, 1).empty());
  for (std::size_t n : {std::size_t{1}, std::size_t{47}, std::size_t{48}, std::size_t{63}, std::size_t{64}, std::size_t{65},
                        std::size_t{127}, std::size_t{128}, std::size_t{129}, std::size_t{1000}}) {
    const auto data = minitest::random_bytes(n, n);
    for (const char* algo : {"gear", "rabin", "fixed"})
      if (!same(cdc::chunk_parallel(algo, p, data.data(), n, 8, 1), sequential(algo, p, data)))
        minitest::fail(__FILE__, __LINE__, std::string(algo) + " differs for n=" + std::to_string(n));
  }
}

TEST(parallel_rejects_unknown_algorithms_and_bad_parameters) {
  const auto data = minitest::random_bytes(1000, 1);
  CHECK_THROWS(cdc::chunk_parallel("nope", cdc::Params(), data.data(), data.size()));
  cdc::Params bad;
  bad.min_size = 8;
  CHECK_THROWS(cdc::chunk_parallel("gear", bad, data.data(), data.size()));
}

// A wrong warm-up at a segment boundary changes the hash at exactly one position per segment, so it
// only matters when that position is a cut candidate. Sweep many segment counts over many inputs with
// a small average size (cuts are frequent) so segment starts land on candidates often.
TEST(parallel_is_exact_at_every_segment_boundary) {
  cdc::Params p;
  p.min_size = 64;
  p.avg_size = 128;
  p.max_size = 1024;
  p.normalization = 1;
  for (std::uint64_t seed = 0; seed < 40; ++seed) {
    const auto data = minitest::random_bytes(3000, 9000 + seed);
    for (const char* algo : {"gear", "rabin"}) {
      const auto expected = sequential(algo, p, data);
      for (unsigned segments = 2; segments <= 30; ++segments)
        if (!same(cdc::chunk_parallel(algo, p, data.data(), data.size(), segments, 1), expected))
          minitest::fail(__FILE__, __LINE__,
                         std::string(algo) + " seed=" + std::to_string(seed) + " segments=" + std::to_string(segments));
    }
  }
}
