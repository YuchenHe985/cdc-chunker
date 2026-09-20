#include <algorithm>

#include "cdc/cdc.hpp"

namespace cdc {

RabinChunker::RabinChunker(const Params& p) : p_(p) {
  p_.validate();
  mask_ = p_.avg_size - 1;
  // Only the last 48 bytes matter for a cut decision, so earlier bytes are skipped unhashed.
  warm_start_ = p_.min_size - detail::kRabinWindow;
}

void RabinChunker::reset() {
  roller_.reset();
  len_ = 0;
  offset_ = 0;
}

void RabinChunker::emit(const Sink& sink) {
  if (sink) sink(offset_, len_);
  offset_ += len_;
  len_ = 0;
  roller_.reset();
}

void RabinChunker::finish(const Sink& sink) {
  if (len_ > 0) emit(sink);
  reset();
}

void RabinChunker::feed(const std::uint8_t* data, std::size_t size, const Sink& sink) {
  const detail::RabinTables& t = detail::rabin_tables();
  std::size_t i = 0;
  while (i < size) {
    if (len_ < warm_start_) {
      const std::size_t k = std::min(size - i, warm_start_ - len_);
      len_ += k;
      i += k;
      continue;
    }
    const bool check = len_ + 1 >= p_.min_size;
    const std::size_t stop = check ? p_.max_size : p_.min_size - 1;
    const std::size_t n = std::min(size - i, stop - len_);
    const std::uint8_t* p = data + i;
    std::size_t k = 0;
    bool cut = false;
    while (k < n) {
      roller_.push(p[k], t);
      ++k;
      if (check && (roller_.digest & mask_) == 0) {
        cut = true;
        break;
      }
    }
    len_ += k;
    i += k;
    if (cut || len_ == p_.max_size) emit(sink);
  }
}

}  // namespace cdc
