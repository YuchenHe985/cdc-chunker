// cdc: content-defined chunking (CDC) with Gear and Rabin rolling hashes.
//
// A chunker splits a byte stream into variable-size chunks whose boundaries depend on the
// content, so inserting or deleting bytes only changes the chunks around the edit. Chunkers are
// streaming: boundaries do not depend on how the input is divided across feed() calls.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cdc {

// Chunk size limits in bytes.
struct Params {
  std::size_t min_size = 2048;   // no cut before this many bytes (>= 64)
  std::size_t avg_size = 8192;   // target size, a power of two
  std::size_t max_size = 65536;  // a cut is forced here
  // Gear only: the cut mask is tightened by this many bits before avg_size and relaxed after it,
  // which narrows the size distribution ("normalized chunking"). 0 disables it.
  int normalization = 2;

  // min = avg / 4, max = avg * 8.
  static Params from_average(std::size_t avg);
  // log2(avg_size).
  int avg_bits() const;
  // Throws std::invalid_argument when the limits are inconsistent.
  void validate() const;
};

// Called once per finished chunk with its stream offset and length.
using Sink = std::function<void(std::uint64_t offset, std::size_t length)>;

struct Chunk {
  std::uint64_t offset;
  std::size_t length;
};

class Chunker {
 public:
  virtual ~Chunker() = default;
  // Consumes bytes and reports every chunk that ends inside them.
  virtual void feed(const std::uint8_t* data, std::size_t size, const Sink& sink) = 0;
  // Reports the final, possibly short, chunk and resets the chunker for a new stream.
  virtual void finish(const Sink& sink) = 0;
  virtual void reset() = 0;
  virtual const char* name() const = 0;
};

namespace detail {

constexpr std::size_t kGearWindow = 64;
constexpr std::size_t kRabinWindow = 48;
// Irreducible over GF(2), degree 53 (verified by tests/cross_check.py).
constexpr std::uint64_t kRabinPoly = 0x3DA3358B4DC173ULL;
// The Gear table is 256 successive splitmix64 outputs from this seed.
constexpr std::uint64_t kGearSeed = 0x243F6A8885A308D3ULL;

std::uint64_t splitmix64(std::uint64_t& state);
constexpr std::uint64_t kFnvOffset = 0xCBF29CE484222325ULL;
// FNV-1a. Passing the previous result as `state` continues a hash across several buffers.
std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size, std::uint64_t state = kFnvOffset);
// The top `bits` bits of a 64-bit word set (0 for bits <= 0).
std::uint64_t top_mask(int bits);

const std::uint64_t* gear_table();
// Definition of the Gear hash of a window: sum over j of table[w[n-1-j]] << j (mod 2^64), n <= 64.
std::uint64_t gear_hash_window(const std::uint8_t* w, std::size_t n);

struct RabinTables {
  std::uint64_t out[256];  // contribution of the byte leaving the window
  std::uint64_t mod[256];  // reduction of the 8 bits shifted above the polynomial degree
  unsigned shift;          // degree - 8
};
const RabinTables& rabin_tables();
// Definition of the Rabin fingerprint: the window as a GF(2) polynomial modulo kRabinPoly,
// computed bit by bit (independent of the table-driven update).
std::uint64_t rabin_hash_window(const std::uint8_t* w /* kRabinWindow bytes */);

// Table-driven rolling update over a window of kRabinWindow bytes.
struct RabinRoller {
  std::uint64_t digest = 0;
  std::uint8_t window[kRabinWindow] = {};
  std::size_t pos = 0;

  void reset() {
    digest = 0;
    std::memset(window, 0, sizeof window);
    pos = 0;
  }
  void push(std::uint8_t b, const RabinTables& t) {
    const std::uint8_t out = window[pos];
    window[pos] = b;
    if (++pos == kRabinWindow) pos = 0;
    digest ^= t.out[out];
    digest = ((digest << 8) | b) ^ t.mod[digest >> t.shift];
  }
};

}  // namespace detail

// Gear hash with cut-point skipping and normalized chunking (after FastCDC, Xia et al., USENIX ATC 2016).
// The cut decision at a position depends only on the previous 64 bytes.
class GearChunker final : public Chunker {
 public:
  explicit GearChunker(const Params& p);
  void feed(const std::uint8_t* data, std::size_t size, const Sink& sink) override;
  void finish(const Sink& sink) override;
  void reset() override;
  const char* name() const override { return "gear"; }

 private:
  void emit(const Sink& sink);
  Params p_;
  std::uint64_t mask_s_, mask_l_;
  std::size_t warm_start_;
  std::uint64_t fp_ = 0;
  std::size_t len_ = 0;
  std::uint64_t offset_ = 0;
};

// Rabin fingerprint over a 48-byte sliding window (after LBFS, Muthitacharoen et al., SOSP 2001).
// The cut decision at a position depends only on the previous 48 bytes.
class RabinChunker final : public Chunker {
 public:
  explicit RabinChunker(const Params& p);
  void feed(const std::uint8_t* data, std::size_t size, const Sink& sink) override;
  void finish(const Sink& sink) override;
  void reset() override;
  const char* name() const override { return "rabin"; }

 private:
  void emit(const Sink& sink);
  Params p_;
  std::uint64_t mask_;
  std::size_t warm_start_;
  detail::RabinRoller roller_;
  std::size_t len_ = 0;
  std::uint64_t offset_ = 0;
};

// Fixed-size blocks of avg_size bytes: the baseline that content-defined chunking is compared with.
class FixedChunker final : public Chunker {
 public:
  explicit FixedChunker(const Params& p);
  void feed(const std::uint8_t* data, std::size_t size, const Sink& sink) override;
  void finish(const Sink& sink) override;
  void reset() override;
  const char* name() const override { return "fixed"; }

 private:
  std::size_t size_;
  std::size_t len_ = 0;
  std::uint64_t offset_ = 0;
};

// "gear", "rabin" or "fixed". Throws std::invalid_argument for anything else.
std::unique_ptr<Chunker> make_chunker(const std::string& algo, const Params& p);

// Resets the chunker, chunks one buffer and returns every chunk.
std::vector<Chunk> chunk_all(Chunker& c, const std::uint8_t* data, std::size_t size);

// Chunks a whole in-memory buffer on several threads. The chunks are identical to what the sequential
// chunker returns for the same algorithm, parameters and buffer, whatever the thread count, so indexes
// built either way are interchangeable.
//
// How: a cut decision depends only on the previous 64 (Gear) or 48 (Rabin) bytes, so every position
// where the hash matches can be found independently in each segment of the buffer. A cheap sequential
// pass then applies min_size, max_size and the strict/relaxed mask rule to that candidate list.
//
// algo is "gear", "rabin" or "fixed". threads == 0 uses the hardware concurrency. A segment is at
// least min_segment bytes, so small buffers use fewer threads.
std::vector<Chunk> chunk_parallel(const std::string& algo, const Params& p, const std::uint8_t* data,
                                  std::size_t size, unsigned threads = 0, std::size_t min_segment = 256 * 1024);

}  // namespace cdc
