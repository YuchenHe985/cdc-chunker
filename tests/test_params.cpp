#include "minitest.hpp"

TEST(params_defaults_and_from_average_are_valid) {
  cdc::Params().validate();
  const cdc::Params p = cdc::Params::from_average(8192);
  CHECK_EQ(p.min_size, std::size_t{2048});
  CHECK_EQ(p.avg_size, std::size_t{8192});
  CHECK_EQ(p.max_size, std::size_t{65536});
  p.validate();
  CHECK_EQ(p.avg_bits(), 13);
  // Small averages keep the minimum at the 64-byte hash window.
  CHECK_EQ(cdc::Params::from_average(128).min_size, std::size_t{64});
  cdc::Params::from_average(128).validate();
}

TEST(params_reject_inconsistent_limits) {
  cdc::Params p;
  p.avg_size = 8000;  // not a power of two
  CHECK_THROWS(p.validate());
  p = cdc::Params();
  p.min_size = 32;  // below the hash window
  CHECK_THROWS(p.validate());
  p = cdc::Params();
  p.min_size = p.avg_size;
  CHECK_THROWS(p.validate());
  p = cdc::Params();
  p.max_size = p.avg_size;
  CHECK_THROWS(p.validate());
  p = cdc::Params();
  p.normalization = 5;
  CHECK_THROWS(p.validate());
  p = cdc::Params();
  p.normalization = -1;
  CHECK_THROWS(p.validate());
  p = cdc::Params();
  p.avg_size = std::size_t{1} << 37;  // a 37-bit mask plus 4 bits of normalization exceeds the 40-bit limit
  p.max_size = std::size_t{1} << 38;
  p.normalization = 4;
  CHECK_THROWS(p.validate());
  p.normalization = 2;
  p.validate();
}

TEST(params_are_validated_by_every_chunker) {
  cdc::Params bad;
  bad.min_size = 10;
  CHECK_THROWS(cdc::GearChunker{bad});
  CHECK_THROWS(cdc::RabinChunker{bad});
  CHECK_THROWS(cdc::make_chunker("nope", cdc::Params()));
  CHECK(cdc::make_chunker("gear", cdc::Params()) != nullptr);
  CHECK(cdc::make_chunker("rabin", cdc::Params()) != nullptr);
  CHECK(cdc::make_chunker("fixed", cdc::Params()) != nullptr);
}
