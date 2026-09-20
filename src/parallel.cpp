#include <algorithm>
#include <exception>
#include <stdexcept>
#include <thread>
#include <vector>

#include "cdc/cdc.hpp"

namespace cdc {

namespace {

// A position where the hash of the window ending at `pos` matches the (relaxed) cut mask.
struct Candidate {
  std::uint64_t pos;  // index of the last byte of the window
  bool strict;        // also matches the strict mask (Gear only; always true for Rabin)
};

// Every window hash is exact because each segment first hashes the bytes just before it, so
// candidates do not depend on where the buffer was split.
void scan_gear(const std::uint8_t* data, std::size_t begin, std::size_t end, std::uint64_t mask_s,
               std::uint64_t mask_l, std::vector<Candidate>& out) {
  const std::uint64_t* g = detail::gear_table();
  std::uint64_t fp = 0;
  constexpr std::size_t kBack = detail::kGearWindow - 1;
  for (std::size_t i = begin >= kBack ? begin - kBack : 0; i < begin; ++i) fp = (fp << 1) + g[data[i]];
  for (std::size_t i = begin; i < end; ++i) {
    fp = (fp << 1) + g[data[i]];
    if ((fp & mask_l) == 0) out.push_back({i, (fp & mask_s) == 0});
  }
}

void scan_rabin(const std::uint8_t* data, std::size_t begin, std::size_t end, std::uint64_t mask,
                std::vector<Candidate>& out) {
  const detail::RabinTables& t = detail::rabin_tables();
  detail::RabinRoller roller;
  constexpr std::size_t kBack = detail::kRabinWindow - 1;
  for (std::size_t i = begin >= kBack ? begin - kBack : 0; i < begin; ++i) roller.push(data[i], t);
  for (std::size_t i = begin; i < end; ++i) {
    roller.push(data[i], t);
    if ((roller.digest & mask) == 0) out.push_back({i, true});
  }
}

struct Joiner {
  std::vector<std::thread>& threads;
  ~Joiner() {
    for (std::thread& t : threads)
      if (t.joinable()) t.join();
  }
};

}  // namespace

std::vector<Chunk> chunk_parallel(const std::string& algo, const Params& p, const std::uint8_t* data,
                                  std::size_t size, unsigned threads, std::size_t min_segment) {
  if (algo == "fixed") {
    const auto chunker = make_chunker(algo, p);
    return chunk_all(*chunker, data, size);
  }
  if (algo != "gear" && algo != "rabin")
    throw std::invalid_argument("unknown algorithm '" + algo + "' (expected gear, rabin or fixed)");
  p.validate();
  if (size == 0) return {};

  if (threads == 0) threads = std::max(1U, std::thread::hardware_concurrency());
  if (min_segment == 0) min_segment = 1;
  const std::size_t segments = std::min<std::size_t>(threads, std::max<std::size_t>(1, size / min_segment));

  const bool gear = algo == "gear";
  const int bits = p.avg_bits();
  const std::uint64_t mask_s = detail::top_mask(bits + p.normalization);
  const std::uint64_t mask_l = detail::top_mask(bits - p.normalization);
  const std::uint64_t rabin_mask = p.avg_size - 1;

  // Stage 1: find every candidate cut position, one segment per thread.
  std::vector<std::vector<Candidate>> found(segments);
  std::vector<std::exception_ptr> errors(segments);
  auto scan_segment = [&](std::size_t s) {
    try {
      const std::size_t begin = size / segments * s + std::min(s, size % segments);
      const std::size_t end = size / segments * (s + 1) + std::min(s + 1, size % segments);
      if (gear)
        scan_gear(data, begin, end, mask_s, mask_l, found[s]);
      else
        scan_rabin(data, begin, end, rabin_mask, found[s]);
    } catch (...) {
      errors[s] = std::current_exception();
    }
  };
  {
    std::vector<std::thread> pool;
    Joiner joiner{pool};
    for (std::size_t s = 1; s < segments; ++s) pool.emplace_back(scan_segment, s);
    scan_segment(0);
  }
  for (const std::exception_ptr& e : errors)
    if (e) std::rethrow_exception(e);

  std::vector<Candidate> cands;
  for (auto& f : found) cands.insert(cands.end(), f.begin(), f.end());

  // Stage 2: walk the chunks in order. A chunk that starts at `start` ends at the first candidate whose
  // length is in [min_size, max_size] and passes the mask rule for that length, else at max_size or the
  // end of the buffer.
  std::vector<Chunk> chunks;
  std::size_t start = 0, next = 0;
  while (start < size) {
    const std::size_t limit = std::min(p.max_size, size - start);
    std::size_t cut = limit;
    while (next < cands.size() && cands[next].pos < start + p.min_size - 1) ++next;
    for (std::size_t k = next; k < cands.size(); ++k) {
      const std::size_t length = static_cast<std::size_t>(cands[k].pos - start) + 1;
      if (length > limit) break;
      if (!gear || length >= p.avg_size || cands[k].strict) {
        cut = length;
        break;
      }
    }
    chunks.push_back({start, cut});
    start += cut;
  }
  return chunks;
}

}  // namespace cdc
