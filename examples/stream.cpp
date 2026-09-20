// Streams a file through a Gear chunker in 64 KiB reads and prints the offset and length of every chunk.
// The chunks are the same however the file is read: try changing the buffer size.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

#include "cdc/cdc.hpp"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: stream FILE\n");
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "stream: cannot open %s\n", argv[1]);
    return 1;
  }
  cdc::GearChunker chunker(cdc::Params::from_average(8192));
  const cdc::Sink print = [](std::uint64_t offset, std::size_t length) {
    std::printf("%llu %zu\n", static_cast<unsigned long long>(offset), length);
  };
  std::vector<char> buffer(64 * 1024);
  while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0)
    chunker.feed(reinterpret_cast<const std::uint8_t*>(buffer.data()), static_cast<std::size_t>(in.gcount()), print);
  chunker.finish(print);
  return 0;
}
