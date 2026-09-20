#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <vector>

#include "minitest.hpp"

namespace {

// Fraction of `b`'s bytes that sit in chunks whose content also appears as a chunk of `a`.
double shared_fraction(const char* algo, const cdc::Params& p, const std::vector<std::uint8_t>& a,
                       const std::vector<std::uint8_t>& b) {
  const auto chunker = cdc::make_chunker(algo, p);
  std::unordered_set<std::uint64_t> known;
  for (const auto& c : cdc::chunk_all(*chunker, a.data(), a.size()))
    known.insert(cdc::detail::fnv1a64(a.data() + c.offset, c.length));
  std::size_t shared = 0;
  for (const auto& c : cdc::chunk_all(*chunker, b.data(), b.size()))
    if (known.count(cdc::detail::fnv1a64(b.data() + c.offset, c.length)) != 0) shared += c.length;
  return static_cast<double>(shared) / static_cast<double>(b.size());
}

struct Stats {
  double mean, cv;
  std::size_t forced;
};

Stats size_stats(const char* algo, const cdc::Params& p, const std::vector<std::uint8_t>& d) {
  const auto chunker = cdc::make_chunker(algo, p);
  const auto chunks = cdc::chunk_all(*chunker, d.data(), d.size());
  double sum = 0, sq = 0;
  std::size_t forced = 0;
  // The final chunk is cut by the end of input, not by content: leave it out.
  for (std::size_t i = 0; i + 1 < chunks.size(); ++i) {
    const double l = static_cast<double>(chunks[i].length);
    sum += l;
    sq += l * l;
    if (chunks[i].length == p.max_size) ++forced;
  }
  const double n = static_cast<double>(chunks.size() - 1);
  const double mean = sum / n;
  return {mean, std::sqrt(sq / n - mean * mean) / mean, forced};
}

}  // namespace

// Fixed-size blocks lose every block after an edit that shifts the data; content-defined
// chunking realigns within a chunk or two.
TEST(chunk_boundaries_survive_an_insertion) {
  const auto a = minitest::random_bytes(4 << 20, 21);
  std::vector<std::uint8_t> b(a.begin(), a.begin() + (1 << 20));
  const auto ins = minitest::random_bytes(100, 22);
  b.insert(b.end(), ins.begin(), ins.end());
  b.insert(b.end(), a.begin() + (1 << 20), a.end());
  const cdc::Params p = cdc::Params::from_average(8192);
  CHECK(shared_fraction("gear", p, a, b) > 0.98);
  CHECK(shared_fraction("rabin", p, a, b) > 0.98);
  CHECK(shared_fraction("fixed", p, a, b) < 0.30);
}

TEST(chunk_boundaries_survive_a_deletion) {
  const auto a = minitest::random_bytes(4 << 20, 23);
  std::vector<std::uint8_t> b(a.begin(), a.begin() + (2 << 20));
  b.insert(b.end(), a.begin() + (2 << 20) + 777, a.end());
  const cdc::Params p = cdc::Params::from_average(8192);
  CHECK(shared_fraction("gear", p, a, b) > 0.98);
  CHECK(shared_fraction("rabin", p, a, b) > 0.98);
  CHECK(shared_fraction("fixed", p, a, b) < 0.55);
}

TEST(identical_inputs_share_everything) {
  const auto a = minitest::random_bytes(1 << 20, 24);
  const cdc::Params p = cdc::Params::from_average(4096);
  for (const char* algo : {"gear", "rabin", "fixed"}) CHECK(shared_fraction(algo, p, a, a) > 0.999999);
}

// On random data the mean chunk length stays within a factor of the target and few chunks hit max_size.
TEST(mean_chunk_size_is_close_to_the_target) {
  const auto d = minitest::random_bytes(16 << 20, 25);
  for (std::size_t avg : {std::size_t{2048}, std::size_t{8192}, std::size_t{32768}}) {
    const cdc::Params p = cdc::Params::from_average(avg);
    for (const char* algo : {"gear", "rabin"}) {
      const Stats s = size_stats(algo, p, d);
      const double ratio = s.mean / static_cast<double>(avg);
      if (!(ratio > 0.8 && ratio < 1.6))
        minitest::fail(__FILE__, __LINE__, std::string(algo) + " mean/avg = " + std::to_string(ratio));
      CHECK(s.forced * 100 < 1 * static_cast<std::size_t>(16 << 20) / avg);  // under 1% of chunks
    }
  }
}

// Normalized chunking is meant to narrow the size distribution; check that it does.
TEST(normalization_narrows_the_size_distribution) {
  const auto d = minitest::random_bytes(16 << 20, 26);
  cdc::Params flat = cdc::Params::from_average(8192);
  flat.normalization = 0;
  cdc::Params norm2 = flat;
  norm2.normalization = 2;
  const Stats a = size_stats("gear", flat, d);
  const Stats b = size_stats("gear", norm2, d);
  CHECK(b.cv < a.cv);
}
