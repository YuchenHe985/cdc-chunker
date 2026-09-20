#include <algorithm>
#include <array>
#include <stdexcept>

#include "cdc/cdc.hpp"

namespace cdc {

Params Params::from_average(std::size_t avg) {
  Params p;
  p.avg_size = avg;
  p.min_size = std::max<std::size_t>(64, avg / 4);
  p.max_size = avg * 8;
  return p;
}

int Params::avg_bits() const {
  int bits = 0;
  for (std::size_t x = avg_size; x > 1; x >>= 1) ++bits;
  return bits;
}

void Params::validate() const {
  if (avg_size < 2 || (avg_size & (avg_size - 1)) != 0)
    throw std::invalid_argument("avg_size must be a power of two");
  if (min_size < detail::kGearWindow) throw std::invalid_argument("min_size must be at least 64");
  if (!(min_size < avg_size && avg_size < max_size))
    throw std::invalid_argument("sizes must satisfy min_size < avg_size < max_size");
  if (normalization < 0 || normalization > 4)
    throw std::invalid_argument("normalization must be between 0 and 4");
  if (avg_bits() - normalization < 1 || avg_bits() + normalization > 40)
    throw std::invalid_argument("avg_size is out of range for this normalization level");
}

namespace detail {

std::uint64_t splitmix64(std::uint64_t& s) {
  s += 0x9E3779B97F4A7C15ULL;
  std::uint64_t z = s;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size, std::uint64_t state) {
  for (std::size_t i = 0; i < size; ++i) {
    state ^= data[i];
    state *= 0x100000001B3ULL;
  }
  return state;
}

std::uint64_t top_mask(int bits) { return bits <= 0 ? 0 : ~std::uint64_t{0} << (64 - bits); }

const std::uint64_t* gear_table() {
  static const std::array<std::uint64_t, 256> table = [] {
    std::array<std::uint64_t, 256> t{};
    std::uint64_t s = kGearSeed;
    for (auto& v : t) v = splitmix64(s);
    return t;
  }();
  return table.data();
}

std::uint64_t gear_hash_window(const std::uint8_t* w, std::size_t n) {
  const std::uint64_t* g = gear_table();
  std::uint64_t h = 0;
  for (std::size_t j = 0; j < n && j < kGearWindow; ++j) h += g[w[n - 1 - j]] << j;
  return h;
}

namespace {

int bit_length(std::uint64_t x) {
  int n = 0;
  for (; x != 0; x >>= 1) ++n;
  return n;
}

// Remainder of a divided by p as polynomials over GF(2).
std::uint64_t poly_mod(std::uint64_t a, std::uint64_t p) {
  const int dp = bit_length(p) - 1;
  for (int b = bit_length(a) - 1; b >= dp; b = bit_length(a) - 1) a ^= p << (b - dp);
  return a;
}

}  // namespace

const RabinTables& rabin_tables() {
  static const RabinTables tables = [] {
    RabinTables t{};
    const int k = bit_length(kRabinPoly) - 1;
    t.shift = static_cast<unsigned>(k - 8);
    for (std::uint64_t b = 0; b < 256; ++b) {
      // out[b] = b * x^(8 * (window - 1)) mod p
      std::uint64_t h = b;
      for (std::size_t i = 0; i + 1 < kRabinWindow; ++i) h = poly_mod(h << 8, kRabinPoly);
      t.out[b] = h;
      const std::uint64_t hi = b << k;
      t.mod[b] = poly_mod(hi, kRabinPoly) | hi;
    }
    return t;
  }();
  return tables;
}

std::uint64_t rabin_hash_window(const std::uint8_t* w) {
  const int k = bit_length(kRabinPoly) - 1;
  std::uint64_t h = 0;
  for (std::size_t i = 0; i < kRabinWindow; ++i) {
    for (int bit = 7; bit >= 0; --bit) {
      h = (h << 1) | ((w[i] >> bit) & 1U);
      if ((h >> k) & 1U) h ^= kRabinPoly;
    }
  }
  return h;
}

}  // namespace detail

FixedChunker::FixedChunker(const Params& p) : size_(p.avg_size) {
  if (size_ == 0) throw std::invalid_argument("avg_size must be positive");
}

void FixedChunker::feed(const std::uint8_t*, std::size_t size, const Sink& sink) {
  std::size_t left = size;
  while (left > 0) {
    const std::size_t take = std::min(left, size_ - len_);
    len_ += take;
    left -= take;
    if (len_ == size_) {
      if (sink) sink(offset_, len_);
      offset_ += len_;
      len_ = 0;
    }
  }
}

void FixedChunker::finish(const Sink& sink) {
  if (len_ > 0 && sink) sink(offset_, len_);
  reset();
}

void FixedChunker::reset() {
  len_ = 0;
  offset_ = 0;
}

std::unique_ptr<Chunker> make_chunker(const std::string& algo, const Params& p) {
  if (algo == "gear") return std::make_unique<GearChunker>(p);
  if (algo == "rabin") return std::make_unique<RabinChunker>(p);
  if (algo == "fixed") return std::make_unique<FixedChunker>(p);
  throw std::invalid_argument("unknown algorithm '" + algo + "' (expected gear, rabin or fixed)");
}

std::vector<Chunk> chunk_all(Chunker& c, const std::uint8_t* data, std::size_t size) {
  c.reset();
  std::vector<Chunk> out;
  const Sink sink = [&out](std::uint64_t offset, std::size_t length) { out.push_back({offset, length}); };
  c.feed(data, size, sink);
  c.finish(sink);
  return out;
}

}  // namespace cdc
