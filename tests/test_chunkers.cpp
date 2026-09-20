#include <algorithm>
#include <string>
#include <vector>

#include "minitest.hpp"

namespace {

struct Config {
  std::string algo;
  cdc::Params params;
  std::string label() const {
    return algo + "(min=" + std::to_string(params.min_size) + ",avg=" + std::to_string(params.avg_size) +
           ",max=" + std::to_string(params.max_size) + ",norm=" + std::to_string(params.normalization) + ")";
  }
};

std::vector<Config> configs() {
  cdc::Params tiny;
  tiny.min_size = 64;
  tiny.avg_size = 128;
  tiny.max_size = 1024;
  tiny.normalization = 1;
  cdc::Params small = cdc::Params::from_average(1024);
  cdc::Params flat = small;
  flat.normalization = 0;
  cdc::Params wide = cdc::Params::from_average(1024);
  wide.normalization = 4;
  const cdc::Params big = cdc::Params::from_average(8192);
  std::vector<Config> out;
  for (const char* algo : {"gear", "rabin", "fixed"})
    for (const cdc::Params& p : {tiny, small, flat, wide, big}) out.push_back({algo, p});
  return out;
}

struct Data {
  std::string name;
  std::vector<std::uint8_t> bytes;
};

std::vector<Data> inputs() {
  std::vector<Data> v;
  v.push_back({"random", minitest::random_bytes(300000, 1)});
  v.push_back({"zeros", std::vector<std::uint8_t>(100000, 0)});
  std::vector<std::uint8_t> periodic(100000);
  for (std::size_t i = 0; i < periodic.size(); ++i) periodic[i] = static_cast<std::uint8_t>("abcdefghij"[i % 10]);
  v.push_back({"periodic", periodic});
  v.push_back({"short", minitest::random_bytes(10, 2)});
  v.push_back({"empty", {}});
  return v;
}

std::vector<cdc::Chunk> run_streaming(cdc::Chunker& c, const std::vector<std::uint8_t>& d, std::uint64_t seed,
                                      std::size_t max_piece) {
  c.reset();
  std::vector<cdc::Chunk> out;
  const cdc::Sink sink = [&out](std::uint64_t offset, std::size_t length) { out.push_back({offset, length}); };
  std::uint64_t state = seed;
  std::size_t pos = 0;
  while (pos < d.size()) {
    const std::size_t take = std::min<std::size_t>(1 + cdc::detail::splitmix64(state) % max_piece, d.size() - pos);
    c.feed(d.data() + pos, take, sink);
    pos += take;
  }
  c.finish(sink);
  return out;
}

bool same(const std::vector<cdc::Chunk>& a, const std::vector<cdc::Chunk>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (a[i].offset != b[i].offset || a[i].length != b[i].length) return false;
  return true;
}

}  // namespace

// Chunks tile the input exactly and respect the size limits.
static void verify_chunks(const std::string& ctx, const Config& cfg, const std::vector<std::uint8_t>& data,
                          const std::vector<cdc::Chunk>& chunks) {
  std::uint64_t next = 0;
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    if (chunks[i].offset != next) minitest::fail(__FILE__, __LINE__, "gap or overlap: " + ctx);
    next += chunks[i].length;
    const bool last = i + 1 == chunks.size();
    if (chunks[i].length == 0) minitest::fail(__FILE__, __LINE__, "empty chunk: " + ctx);
    if (cfg.algo == "fixed") {
      if (chunks[i].length > cfg.params.avg_size || (!last && chunks[i].length != cfg.params.avg_size))
        minitest::fail(__FILE__, __LINE__, "fixed chunk size: " + ctx);
    } else {
      if (chunks[i].length > cfg.params.max_size) minitest::fail(__FILE__, __LINE__, "above max: " + ctx);
      if (!last && chunks[i].length < cfg.params.min_size) minitest::fail(__FILE__, __LINE__, "below min: " + ctx);
    }
  }
  if (next != data.size()) minitest::fail(__FILE__, __LINE__, "chunks do not sum to the input: " + ctx);
}

TEST(chunks_cover_the_input_and_respect_limits) {
  for (const Config& cfg : configs()) {
    for (const Data& d : inputs()) {
      const auto chunker = cdc::make_chunker(cfg.algo, cfg.params);
      verify_chunks(cfg.label() + " on " + d.name, cfg, d.bytes, cdc::chunk_all(*chunker, d.bytes.data(), d.bytes.size()));
    }
  }
}

TEST(empty_and_short_inputs) {
  for (const Config& cfg : configs()) {
    const auto chunker = cdc::make_chunker(cfg.algo, cfg.params);
    CHECK(cdc::chunk_all(*chunker, nullptr, 0).empty());
    const auto one = minitest::random_bytes(cfg.params.min_size - 1, 3);
    const auto chunks = cdc::chunk_all(*chunker, one.data(), one.size());
    CHECK_EQ(chunks.size(), std::size_t{1});
    CHECK_EQ(chunks[0].length, one.size());
  }
}

// The property that makes the chunkers usable on streams: how the input is split across feed()
// calls never changes the boundaries.
TEST(boundaries_do_not_depend_on_how_input_is_fed) {
  const auto data = minitest::random_bytes(200000, 5);
  for (const Config& cfg : configs()) {
    const auto chunker = cdc::make_chunker(cfg.algo, cfg.params);
    for (std::uint64_t seed = 1; seed <= 4; ++seed)
      for (std::size_t piece : {std::size_t{1}, std::size_t{7}, std::size_t{100}, std::size_t{5000}, std::size_t{70000}}) {
        // One-byte pieces on the whole input would be slow to check for every config; use a prefix.
        const std::vector<std::uint8_t> input(data.begin(), data.begin() + (piece == 1 ? 30000 : 200000));
        const auto expected = cdc::chunk_all(*chunker, input.data(), input.size());
        if (!same(run_streaming(*chunker, input, seed, piece), expected))
          minitest::fail(__FILE__, __LINE__, "streaming differs: " + cfg.label() + " piece<=" + std::to_string(piece));
      }
  }
}

TEST(finish_resets_and_a_chunker_can_be_reused) {
  const auto data = minitest::random_bytes(100000, 6);
  for (const Config& cfg : configs()) {
    const auto chunker = cdc::make_chunker(cfg.algo, cfg.params);
    const auto first = cdc::chunk_all(*chunker, data.data(), data.size());
    const auto second = cdc::chunk_all(*chunker, data.data(), data.size());
    CHECK(same(first, second));
    // Without an explicit reset() in between, finish() alone must leave a clean state.
    std::vector<cdc::Chunk> third;
    const cdc::Sink sink = [&third](std::uint64_t o, std::size_t l) { third.push_back({o, l}); };
    chunker->feed(data.data(), data.size(), sink);
    chunker->finish(sink);
    third.clear();
    chunker->feed(data.data(), data.size(), sink);
    chunker->finish(sink);
    CHECK(same(first, third));
  }
}

TEST(a_null_sink_is_allowed) {
  const auto data = minitest::random_bytes(50000, 7);
  for (const Config& cfg : configs()) {
    const auto chunker = cdc::make_chunker(cfg.algo, cfg.params);
    chunker->feed(data.data(), data.size(), cdc::Sink());
    chunker->finish(cdc::Sink());
  }
}

// With an all-zero window the Rabin digest is zero, so every chunk ends at exactly min_size.
TEST(rabin_on_zeros_cuts_at_min_size) {
  cdc::Params p = cdc::Params::from_average(1024);
  cdc::RabinChunker chunker(p);
  const std::vector<std::uint8_t> zeros(20 * p.min_size, 0);
  const auto chunks = cdc::chunk_all(chunker, zeros.data(), zeros.size());
  CHECK_EQ(chunks.size(), std::size_t{20});
  for (const auto& c : chunks) CHECK_EQ(c.length, p.min_size);
}

// With constant input the Gear hash is constant once the window is full, and for this table it never
// matches the cut mask, so every chunk ends at the forced cut at max_size.
TEST(gear_forces_a_cut_at_max_size_when_no_cut_point_matches) {
  cdc::Params p = cdc::Params::from_average(1024);
  p.normalization = 4;
  cdc::GearChunker chunker(p);
  const std::vector<std::uint8_t> data(200000, 0xFF);
  const auto chunks = cdc::chunk_all(chunker, data.data(), data.size());
  CHECK(chunks.size() > 10);
  std::size_t total = 0;
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    total += chunks[i].length;
    if (i + 1 < chunks.size()) CHECK_EQ(chunks[i].length, p.max_size);
  }
  CHECK_EQ(total, data.size());
}

// The first place a cut can happen is exactly min_size, and the decision there depends on all of the
// last 64 (Gear) or 48 (Rabin) bytes, including the oldest one. Compare the chunker with the hash
// computed from the definition on many random blocks.
TEST(decision_at_min_size_matches_the_hash_definition) {
  for (std::size_t min_size : {std::size_t{64}, std::size_t{100}}) {
    cdc::Params p;
    p.min_size = min_size;
    p.avg_size = 128;
    p.max_size = 1024;
    p.normalization = 1;
    const int bits = p.avg_bits();
    const std::uint64_t strict = ~std::uint64_t{0} << (64 - (bits + p.normalization));
    const std::uint64_t rabin_mask = p.avg_size - 1;
    std::size_t gear_cuts = 0, rabin_cuts = 0;
    for (std::uint64_t trial = 0; trial < 30000; ++trial) {
      std::vector<std::uint8_t> data = minitest::random_bytes(min_size + 300, 1000 + trial);
      cdc::GearChunker gear(p);
      const bool gear_expected =
          (cdc::detail::gear_hash_window(&data[min_size - cdc::detail::kGearWindow], cdc::detail::kGearWindow) & strict) == 0;
      const bool gear_cut = cdc::chunk_all(gear, data.data(), data.size())[0].length == min_size;
      if (gear_cut != gear_expected) minitest::fail(__FILE__, __LINE__, "gear decision at min_size, trial " + std::to_string(trial));
      gear_cuts += gear_expected ? 1 : 0;

      cdc::RabinChunker rabin(p);
      const bool rabin_expected =
          (cdc::detail::rabin_hash_window(&data[min_size - cdc::detail::kRabinWindow]) & rabin_mask) == 0;
      const bool rabin_cut = cdc::chunk_all(rabin, data.data(), data.size())[0].length == min_size;
      if (rabin_cut != rabin_expected) minitest::fail(__FILE__, __LINE__, "rabin decision at min_size, trial " + std::to_string(trial));
      rabin_cuts += rabin_expected ? 1 : 0;
    }
    // The comparison is only meaningful if cuts at min_size actually occur.
    CHECK(gear_cuts > 50);
    CHECK(rabin_cuts > 100);
  }
}

// The hand-picked configurations above are not the whole parameter space. Random valid parameters,
// random content (random, zeros, repeated blocks) and random feed sizes must all satisfy the same
// invariants, and feeding in pieces must not change the boundaries.
TEST(random_parameters_content_and_feed_sizes) {
  std::uint64_t state = 20260920;
  auto rnd = [&state](std::uint64_t n) { return cdc::detail::splitmix64(state) % n; };
  for (int iter = 0; iter < 300; ++iter) {
    Config cfg;
    cfg.algo = std::vector<std::string>{"gear", "rabin", "fixed"}[rnd(3)];
    cfg.params.min_size = 64 + rnd(1500);
    cfg.params.avg_size = std::size_t{128} << rnd(7);
    while (cfg.params.avg_size <= cfg.params.min_size) cfg.params.avg_size <<= 1;
    cfg.params.max_size = cfg.params.avg_size * (2 + rnd(7));
    cfg.params.normalization = static_cast<int>(rnd(5));
    cfg.params.validate();

    const std::size_t n = rnd(80000);
    std::vector<std::uint8_t> data;
    switch (rnd(3)) {
      case 0: data = minitest::random_bytes(n, rnd(1000000)); break;
      case 1: data.assign(n, static_cast<std::uint8_t>(rnd(256))); break;
      default: {
        const auto block = minitest::random_bytes(1 + rnd(3000), rnd(1000000));
        for (std::size_t i = 0; i < n; ++i) data.push_back(block[i % block.size()]);
      }
    }
    const std::string ctx = cfg.label() + " n=" + std::to_string(n);
    const auto chunker = cdc::make_chunker(cfg.algo, cfg.params);
    const auto whole = cdc::chunk_all(*chunker, data.data(), data.size());
    verify_chunks(ctx, cfg, data, whole);
    if (!same(run_streaming(*chunker, data, rnd(1000000), 1 + rnd(9000)), whole))
      minitest::fail(__FILE__, __LINE__, "streaming differs: " + ctx);
  }
}
