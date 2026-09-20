#include <set>
#include <string>

#include "minitest.hpp"

using namespace cdc::detail;

TEST(splitmix64_and_fnv_match_published_vectors) {
  std::uint64_t s = 0;
  CHECK_EQ(splitmix64(s), std::uint64_t{0xE220A8397B1DCDAFULL});
  CHECK_EQ(splitmix64(s), std::uint64_t{0x6E789E6AA1B965F4ULL});
  CHECK_EQ(splitmix64(s), std::uint64_t{0x06C45D188009454FULL});
  const std::string a = "a", foobar = "foobar";
  CHECK_EQ(fnv1a64(nullptr, 0), std::uint64_t{0xCBF29CE484222325ULL});
  CHECK_EQ(fnv1a64(reinterpret_cast<const std::uint8_t*>(a.data()), a.size()), std::uint64_t{0xAF63DC4C8601EC8CULL});
  CHECK_EQ(fnv1a64(reinterpret_cast<const std::uint8_t*>(foobar.data()), foobar.size()),
           std::uint64_t{0x85944171F73967E8ULL});
}

TEST(gear_table_entries_are_distinct) {
  std::set<std::uint64_t> seen(gear_table(), gear_table() + 256);
  CHECK_EQ(seen.size(), std::size_t{256});
}

// The rolling update used by the chunker equals the definition of the hash of the last 64 bytes.
TEST(gear_rolling_hash_equals_its_definition) {
  const auto data = minitest::random_bytes(5000, 11);
  const std::uint64_t* g = gear_table();
  std::uint64_t fp = 0;
  for (std::size_t i = 0; i < data.size(); ++i) {
    fp = (fp << 1) + g[data[i]];
    const std::size_t n = std::min<std::size_t>(i + 1, kGearWindow);
    CHECK_EQ(fp, gear_hash_window(&data[i + 1 - n], n));
  }
}

// The table-driven Rabin update equals the bit-serial polynomial remainder of the window.
TEST(rabin_rolling_hash_equals_its_definition) {
  const auto data = minitest::random_bytes(5000, 12);
  const RabinTables& t = rabin_tables();
  RabinRoller r;
  for (std::size_t i = 0; i < data.size(); ++i) {
    r.push(data[i], t);
    CHECK(r.digest < (std::uint64_t{1} << 53));
    if (i + 1 >= kRabinWindow) CHECK_EQ(r.digest, rabin_hash_window(&data[i + 1 - kRabinWindow]));
  }
}

TEST(rabin_digest_forgets_bytes_older_than_the_window) {
  auto a = minitest::random_bytes(400, 13);
  auto b = a;
  for (std::size_t i = 0; i < 300; ++i) b[i] = static_cast<std::uint8_t>(b[i] ^ 0x5A);  // differ only far back
  const RabinTables& t = rabin_tables();
  RabinRoller ra, rb;
  for (std::size_t i = 0; i < a.size(); ++i) {
    ra.push(a[i], t);
    rb.push(b[i], t);
  }
  CHECK_EQ(ra.digest, rb.digest);
}

TEST(rabin_window_of_zeros_has_digest_zero) {
  const std::uint8_t zeros[kRabinWindow] = {};
  CHECK_EQ(rabin_hash_window(zeros), std::uint64_t{0});
}
