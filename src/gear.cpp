#include <algorithm>

#include "cdc/cdc.hpp"

namespace cdc {

using detail::top_mask;

GearChunker::GearChunker(const Params& p) : p_(p) {
  p_.validate();
  const int bits = p_.avg_bits();
  mask_s_ = top_mask(bits + p_.normalization);
  mask_l_ = top_mask(bits - p_.normalization);
  // Bytes before min_size - 64 cannot influence a cut decision (the hash forgets them after 64
  // bytes), so they are skipped without hashing.
  warm_start_ = p_.min_size - detail::kGearWindow;
}

void GearChunker::reset() {
  fp_ = 0;
  len_ = 0;
  offset_ = 0;
}

void GearChunker::emit(const Sink& sink) {
  if (sink) sink(offset_, len_);
  offset_ += len_;
  len_ = 0;
  fp_ = 0;
}

void GearChunker::finish(const Sink& sink) {
  if (len_ > 0) emit(sink);
  reset();
}

void GearChunker::feed(const std::uint8_t* data, std::size_t size, const Sink& sink) {
  const std::uint64_t* g = detail::gear_table();
  std::size_t i = 0;
  while (i < size) {
    if (len_ < warm_start_) {
      const std::size_t k = std::min(size - i, warm_start_ - len_);
      len_ += k;
      i += k;
      continue;
    }
    // Three regions by chunk length L after adding a byte: L < min (hash only), min <= L < avg
    // (strict mask), avg <= L <= max (relaxed mask; a cut is forced at max).
    bool check = true;
    std::uint64_t mask = mask_l_;
    std::size_t stop = p_.max_size;
    if (len_ + 1 < p_.min_size) {
      check = false;
      stop = p_.min_size - 1;
    } else if (len_ + 1 < p_.avg_size) {
      mask = mask_s_;
      stop = p_.avg_size - 1;
    }
    const std::size_t n = std::min(size - i, stop - len_);
    const std::uint8_t* p = data + i;
    std::uint64_t fp = fp_;
    std::size_t k = 0;
    bool cut = false;
    if (!check) {
      for (; k < n; ++k) fp = (fp << 1) + g[p[k]];
    } else {
      while (k < n) {
        fp = (fp << 1) + g[p[k]];
        ++k;
        if ((fp & mask) == 0) {
          cut = true;
          break;
        }
      }
    }
    fp_ = fp;
    len_ += k;
    i += k;
    if (cut || len_ == p_.max_size) emit(sink);
  }
}

}  // namespace cdc
