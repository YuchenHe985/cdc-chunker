# cdc-chunker

[![ci](https://github.com/YuchenHe985/cdc-chunker/actions/workflows/ci.yml/badge.svg)](https://github.com/YuchenHe985/cdc-chunker/actions/workflows/ci.yml)

cdc-chunker splits a byte stream into variable-size chunks whose boundaries depend on the content, so an edit changes only
the chunks around it. Deduplicating storage, incremental backup and delta sync are built on this property; with fixed-size
blocks, one inserted byte changes every block after it.

It provides two chunkers behind one streaming interface, Gear (FastCDC-style, with normalized chunk sizes) and Rabin, a
command line tool that reports how much of a new file version is already stored, and a benchmark harness. C++17, standard
library only.

## Design goals

| Goal | How it is met | Evidence |
| --- | --- | --- |
| Boundaries survive edits | a cut depends only on the previous 64 (Gear) or 48 (Rabin) bytes | insertion and deletion tests; after 200 edits to a 67 MB tree, 96.5% still deduplicates, against 1.6% for fixed blocks |
| Chunk sizes are bounded and predictable | `min` / `avg` / `max` limits; normalized masks narrow the spread | property tests; coefficient of variation 0.30 |
| Works on streams of unknown length with any buffer size | constant-size state, `feed()` / `finish()` | tests feeding 1 byte to 70 KB at a time give identical chunks |
| Chunking is not the bottleneck | bytes that cannot affect a cut are skipped; one table lookup per byte | about 2 GB/s on one core, above the 1.25 GB/s line rate of 10 GbE |
| Chunk indexes stay valid across builds and machines | fixed constants, integer arithmetic only | golden vectors; identical chunk lengths on x86-64 Linux and arm64 macOS in CI |
| Bugs do not hide behind lucky tests | independent reference implementation, injected-bug check | chunk-for-chunk agreement on 11 inputs; 18 of 18 injected bugs caught |

## Results

Apple M1, one thread, Apple clang 16, `-O3`. Reproduce with `./build/cdc_bench all --corpus <dir>`.

- **Gear chunks at about 2 GB/s, 4.7x faster than Rabin** (0.42 GB/s), and about 2.5x the speed of a byte-at-a-time FNV-1a pass over the same buffer (0.80 GB/s), which is the reference cost of fingerprinting every byte.
- **After 200 random edits to a 67 MB source tree, content-defined chunking still shares 93-96% of the data with the previous version at 8 KiB chunks (98-99% at 2 KiB); fixed-size blocks share 1.6%.**
- **Normalized chunking narrows the chunk size spread from a coefficient of variation of 0.80 to 0.30**, which cuts the data lost per edit by about 40% (20.1 to 11.8 KB per edit at 8 KiB chunks).

### Throughput

256 MiB of pseudo-random bytes, median of 9 runs after a warm-up (the machine was not otherwise idle; min and max show the spread).

| chunker | avg size | median MB/s | min | max |
| --- | ---: | ---: | ---: | ---: |
| rabin | 2048 | 413 | 409 | 417 |
| gear (norm 0) | 2048 | 1885 | 973 | 1899 |
| gear (norm 2) | 2048 | 1933 | 1913 | 1939 |
| rabin | 8192 | 422 | 416 | 426 |
| gear (norm 0) | 8192 | 1931 | 1887 | 1965 |
| gear (norm 2) | 8192 | 1973 | 1949 | 1983 |
| rabin | 32768 | 421 | 418 | 424 |
| gear (norm 0) | 32768 | 1937 | 1934 | 1954 |
| gear (norm 2) | 32768 | 1989 | 1976 | 2012 |
| FNV-1a 64 over the buffer (reference) | - | 798 | 797 | 799 |

### Chunk size distribution

64 MiB of pseudo-random bytes; `min = avg/4`, `max = avg*8`; cv is stddev / mean.

| chunker | target avg | mean | cv | p5 | p50 | p95 | cut at max |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| rabin | 8192 | 10394 | 0.80 | 2480 | 7786 | 27430 | 0.03% |
| gear (norm 0) | 8192 | 10192 | 0.79 | 2451 | 7826 | 26838 | 0.06% |
| gear (norm 2) | 8192 | 9404 | 0.30 | 3791 | 9251 | 14040 | 0.00% |

The mean sits above the target because nothing is cut before `min_size`: without normalization it is about
`min_size + avg_size`. The 2 KiB and 32 KiB rows are in the benchmark output and behave the same way.

### Deduplication after edits

Version 1 is the first 64 MiB (67.1 MB, 5,359 files) of the Go 1.27.1 standard library source, concatenated in path order. Version 2 applies 200
random insertions, deletions and overwrites of 1-64 bytes (seed 1). "Shared" is the fraction of version 2 that lies in chunks
byte-identical to a chunk of version 1.

| chunker | avg size | mean chunk | shared with v1 | new bytes per edit |
| --- | ---: | ---: | ---: | ---: |
| fixed | 2048 | 2048 | 1.70% | 329.8 KB |
| rabin | 2048 | 2712 | 98.36% | 5.5 KB |
| gear (norm 0) | 2048 | 2726 | 98.46% | 5.2 KB |
| gear (norm 2) | 2048 | 2381 | 99.08% | 3.1 KB |
| fixed | 8192 | 8191 | 1.59% | 330.2 KB |
| rabin | 8192 | 11407 | 92.81% | 24.1 KB |
| gear (norm 0) | 8192 | 10967 | 94.00% | 20.1 KB |
| gear (norm 2) | 8192 | 9836 | 96.49% | 11.8 KB |
| fixed | 32768 | 32752 | 1.42% | 330.8 KB |
| rabin | 32768 | 47327 | 74.84% | 84.4 KB |
| gear (norm 0) | 32768 | 46962 | 78.30% | 72.8 KB |
| gear (norm 2) | 32768 | 39360 | 86.87% | 44.1 KB |

How to read it: at the same target size the algorithms end up with different mean chunk sizes, so the comparison is not
at equal chunk size. An edit destroys the chunk it lands in, and a random edit lands in a chunk in proportion to its size,
so the expected loss is about `mean * (1 + cv^2)`; the wider Rabin distribution (cv 0.8) loses more per edit than the
narrow normalized Gear distribution (cv 0.3) even at similar means. The observed 11.8 KB against a predicted 10.7 KB for
normalized Gear, and 24.1 KB against 18.7 KB for Rabin, agree in order but not exactly; edits that land near a boundary
also disturb the neighbouring chunk. Smaller chunks dedupe better but cost more index entries.

Caveats: the throughput data is random bytes (no cache-friendly repetition), the dedup data is source code with synthetic
edits, and chunk identity is a 64-bit FNV-1a hash, adequate for measuring overlap and not for a real store.

## Use

```cpp
#include "cdc/cdc.hpp"

cdc::GearChunker chunker(cdc::Params::from_average(8192));           // min 2 KiB, avg 8 KiB, max 64 KiB
const cdc::Sink print = [](std::uint64_t offset, std::size_t length) { /* one finished chunk */ };
chunker.feed(block1, n1, print);                                      // any block sizes, boundaries are unaffected
chunker.feed(block2, n2, print);
chunker.finish(print);                                                // reports the last chunk
```

[examples/stream.cpp](examples/stream.cpp) is a complete program. Build and try the tools:

```bash
cmake -S . -B build && cmake --build build -j
ctest --test-dir build                       # unit tests and the cross-check against the reference

./build/cdc stats --algo gear --avg 8192 file.bin
# algo=gear bytes=16777216 chunks=1768
# mean=9489 stddev=2818 cv=0.30 min=2050 p5=4231 p50=9306 p95=14036 max=26066 forced_at_max=0

./build/cdc chunk --algo rabin --avg 8192 file.bin        # offset, length, hash per chunk
./build/cdc dedup --algo gear --avg 8192 old.bin new.bin  # how much of new.bin is already in old.bin
```

`Params` takes `min_size`, `avg_size` (a power of two), `max_size` and, for Gear, a `normalization` level (0-4).
Invalid limits throw `std::invalid_argument`.

## How it is tested

| Check | What it establishes |
| --- | --- |
| 26 unit tests | chunks tile the input and respect the limits; boundaries do not depend on how input is fed (1-byte to 70 KB pieces); rolling hashes equal the hash computed from the window; boundaries survive insertions and deletions; sizes match the target; the decision exactly at `min_size` matches the definition; 300 random parameter sets, contents and feed sizes satisfy the same invariants |
| Differential test | the library's chunk lengths equal those of `tests/reference.py` on 11 inputs and parameter sets. The reference computes every cut from the definition with no rolling state and shares no code with the library. It also checks that the Rabin polynomial is irreducible |
| Golden vectors | the boundaries of a fixed input do not change, which would invalidate stored chunk indexes |
| Mutation check | `tests/mutation_check.py` injects 18 bugs (off-by-one, swapped masks, wrong shift, ...) and confirms the tests catch each; two edits that cannot matter, because the sliding window makes the state irrelevant, survive as expected |
| CI | gcc and clang on Linux (x86-64), clang on macOS (arm64), all with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`; the cross-check runs on each, so chunk boundaries are identical across the two architectures; an AddressSanitizer and UBSan job; the mutation check |

## Status and limits

Version 0.1.0. Single-threaded, no SIMD. Tested on Linux (x86-64) and macOS (arm64), not on Windows. A chunker object holds
per-stream state, so use one per stream. The `cdc` tool reads whole files into memory. The API may change before 1.0.

Design notes, deviations from the papers, and limits: [docs/DESIGN.md](docs/DESIGN.md).

## Layout

```
include/cdc/cdc.hpp   public API: Params, Chunker, GearChunker, RabinChunker, FixedChunker
src/                  implementations
tools/cdc_cli.cpp     chunk, stats, dedup, info
bench/bench.cpp       throughput, size distribution, dedup after edits
examples/stream.cpp   streaming example
tests/                unit tests, reference implementation, cross-check, mutation check
```

## Provenance and licence

Written from the published algorithms: Gear hashing and normalized chunking from FastCDC (Xia et al., USENIX ATC 2016) and
Rabin fingerprinting from LBFS (Muthitacharoen et al., SOSP 2001). No third-party code; the Gear table is generated in
code, so chunk boundaries will not match other implementations. MIT licensed.
