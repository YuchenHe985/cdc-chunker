#include <vector>

#include "minitest.hpp"

// Chunk lengths for a fixed input, produced by the definition-based reference (tests/reference.py).
// If a change to the algorithms moves any boundary, these fail, which is what a stored index of
// chunk hashes would suffer from as well.
namespace {

const std::vector<std::uint8_t>& input() {
  static const std::vector<std::uint8_t> d = minitest::random_bytes(200000, 42);
  return d;
}

void expect_prefix(const char* algo, const cdc::Params& p, std::size_t total_chunks,
                   const std::vector<std::size_t>& first) {
  const auto chunker = cdc::make_chunker(algo, p);
  const auto chunks = cdc::chunk_all(*chunker, input().data(), input().size());
  CHECK_EQ(chunks.size(), total_chunks);
  for (std::size_t i = 0; i < first.size() && i < chunks.size(); ++i) CHECK_EQ(chunks[i].length, first[i]);
}

}  // namespace

TEST(golden_gear_with_normalization) {
  cdc::Params p = cdc::Params::from_average(1024);
  expect_prefix("gear", p, 170, {1026, 1126, 1579, 785, 1661, 1172, 1251, 1277, 1105, 1082, 1189, 942, 1282, 1257});
}

TEST(golden_gear_without_normalization) {
  cdc::Params p = cdc::Params::from_average(1024);
  p.normalization = 0;
  expect_prefix("gear", p, 160, {4516, 770, 1700, 1614, 1487, 420, 2983, 705, 1714, 1689, 1833, 642, 1263, 1700});
}

TEST(golden_rabin) {
  expect_prefix("rabin", cdc::Params::from_average(1024), 152,
                {532, 3860, 338, 2400, 3119, 482, 439, 484, 5273, 699, 3567, 2918, 408, 256});
}
