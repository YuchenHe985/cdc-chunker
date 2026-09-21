# cdc-chunker

[![ci](https://github.com/YuchenHe985/cdc-chunker/actions/workflows/ci.yml/badge.svg)](https://github.com/YuchenHe985/cdc-chunker/actions/workflows/ci.yml) ![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=c%2B%2B&logoColor=white) [![License: MIT](https://img.shields.io/badge/License-MIT-2ea44f.svg)](LICENSE)

cdc-chunker splits a byte stream into variable-size chunks whose boundaries depend on the content, so an edit changes only
the chunks around it. Deduplicating storage, incremental backup and delta sync are built on this property; with fixed-size
blocks, one inserted byte changes every block after it.

It provides two chunkers behind one streaming interface, Gear (FastCDC-style, with normalized chunk sizes) and Rabin, a
multi-threaded mode that returns exactly the same chunks as the sequential one, a command line tool that reports how much of
a new file version is already stored, and a benchmark harness. C++17, standard library only.

> **Measured on Apple M1:** about **2.0 GB/s** sequential and **7.4 GB/s** with 8 threads; **96.5%** reuse after 200 random edits at an 8 KiB target chunk size; **31 tests** plus a **23/23 mutation check**.

Where it matters: chunking sits on the write path of deduplicating storage, so it has to keep up with the disks or the
network. Model hubs are a current example: Hugging Face's Xet storage splits model and dataset files with a Gear-based
content-defined chunker (minimum 8 KiB, target about 64 KiB, maximum 128 KiB, see its
[chunking spec](https://huggingface.co/docs/xet/en/chunking)) and keeps only the chunks it has not seen before. Those limits
are valid `Params` here (`min_size = 8192`, `avg_size = 65536`, `max_size = 131072`); the boundaries will not match Xet's,
because the hash table differs.

## Design goals

| Goal | How it is met | Evidence |
| --- | --- | --- |
| Boundaries survive edits | a cut depends only on the previous 64 (Gear) or 48 (Rabin) bytes | insertion and deletion tests; after 200 edits to a 67 MB tree, 96.5% still deduplicates, against 1.6% for fixed blocks |
| Chunk sizes are bounded and predictable | `min` / `avg` / `max` limits; normalized masks narrow the spread | property tests; coefficient of variation 0.30 |
| Works on streams of unknown length with any buffer size | constant-size state, `feed()` / `finish()` | tests feeding 1 byte to 70 KB at a time give identical chunks |
| Chunking is not the bottleneck | bytes that cannot affect a cut are skipped; one table lookup per byte; `chunk_parallel` uses all cores and returns the same chunks | about 2 GB/s on one core, 7.4 GB/s on 8 threads, against the 1.25 GB/s line rate of 10 GbE |
| Chunk indexes stay valid across builds, machines and thread counts | fixed constants, integer arithmetic only; parallel and sequential chunks are identical by construction | golden vectors; identical chunk lengths on x86-64 Linux and arm64 macOS in CI; 500 random cases plus a sweep of segment boundaries compare parallel with sequential |
| Bugs do not hide behind lucky tests | independent reference implementation, injected-bug check, ThreadSanitizer | chunk-for-chunk agreement on 11 inputs, single- and multi-threaded; 23 of 23 injected bugs caught |

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

### Multi-core chunking

`chunk_parallel` on a 256 MiB in-memory buffer, avg 8 KiB, median of 9 runs (8 hardware threads: 4 performance and 4 efficiency
cores; the machine was not otherwise idle). Every multi-threaded result was compared with the sequential chunks by the
benchmark itself and had to be identical.

| chunker | threads | median MB/s | speedup vs sequential |
| --- | ---: | ---: | ---: |
| gear (norm 2) | sequential | 1966 | 1.00x |
| gear (norm 2) | 1 | 1497 | 0.76x |
| gear (norm 2) | 2 | 2911 | 1.48x |
| gear (norm 2) | 4 | 5565 | 2.83x |
| gear (norm 2) | 8 | 7388 | 3.76x |
| rabin | sequential | 423 | 1.00x |
| rabin | 1 | 216 | 0.51x |
| rabin | 2 | 418 | 0.99x |
| rabin | 4 | 800 | 1.89x |
| rabin | 8 | 1167 | 2.76x |

The parallel path is slower on one thread because it cannot skip the bytes at the start of each chunk (it does not know where
chunks start until the second stage), so it pays off from two threads. Scaling is sublinear on this machine.

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

### Deduplication of model files

A question a chunking store faces every time someone fine-tunes a model: how much of the new file is already stored? The base is
[Qwen2.5-0.5B-Instruct](https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct) (BF16, cast to fp16); the fine-tuned model is the same
network after LoRA (rank 16 on every attention and MLP projection) was merged into the weights and saved as fp16, from
[llm-finetune-lab](https://github.com/YuchenHe985/llm-finetune-lab). The GGUF files come from llama.cpp's converter and `llama-quantize`, run on each
model in the same way. Chunker: `cdc dedup --algo gear --min 8192 --avg 65536 --max 131072 BASE FINETUNED`, the limits Hugging Face's Xet uses. "Shared" is the
fraction of the fine-tuned file that lies in chunks byte-identical to a chunk of the base file.

| Format | Fine-tuned size | Shared with base | What is shared |
| --- | ---: | ---: | --- |
| safetensors as published (base BF16, fine-tuned F16) | 988 MB | 0.0% | nothing: the dtypes differ |
| safetensors, both F16 | 988 MB | 27.6% | the token embedding |
| GGUF F16 | 994 MB | 28.0% | embedding and the tokenizer stored in the file (5.8 MB) |
| GGUF Q8_0 | 531 MB | 28.3% | the same |
| GGUF Q4_K_M | 398 MB | 37.8% | the same; the embedding stays 8-bit (145 MB) while the rest shrinks |

`tools/compare_tensors.py` shows why, tensor group by tensor group (safetensors, both F16; identical means the same bytes):

| tensor group | size | elements identical |
| --- | ---: | ---: |
| token embedding | 272 MB | 100% |
| MLP projections (gate, up, down) | 628 MB | 0.7-1.5% |
| attention projections (q, k, v, o) | 88 MB | 0.6-1.1% |
| norms and biases | 0.1 MB | 100% |

- **Only untouched tensors deduplicate.** LoRA changed about 99% of the values in every projection matrix, so chunks inside them never match: the store
  reuses the embedding (and, in GGUF, the tokenizer) and takes the other 716 MB (72%) as new. The adapter alone is 8.8M parameters, about 35 MB in fp32, which is what
  to ship or store if the base is already there.
- **The dtype decides more than the chunker does.** The unchanged 272 MB embedding is fully shared when both files are fp16 and shares nothing when one
  side is BF16. Check dtypes before expecting any reuse.
- **Quantization shrinks what can be shared.** The embedding drops from 272 MB to 145 MB, so the bytes saved fall from 278 MB (GGUF F16) to 150 MB
  (Q8_0 and Q4_K_M); Q4_K_M's 37.8% is higher only because the rest of the file shrank more.
- Both files here have the same size and layout, so fixed 64 KiB blocks also find 27.6%; content-defined boundaries matter when offsets shift, as in the
  edit experiment above. Swapping the two files gives the same figure, and a file against itself gives 100%.

Caveats: one model and one LoRA configuration (the embedding was not trained; a full fine-tune or a LoRA that includes the embedding would share less);
the Gear tables here are not Xet's, so boundaries differ from what Xet computes; the BF16 to fp16 cast is mine.

## Use

```cpp
#include "cdc/cdc.hpp"

cdc::GearChunker chunker(cdc::Params::from_average(8192));           // min 2 KiB, avg 8 KiB, max 64 KiB
const cdc::Sink print = [](std::uint64_t offset, std::size_t length) { /* one finished chunk */ };
chunker.feed(block1, n1, print);                                      // any block sizes, boundaries are unaffected
chunker.feed(block2, n2, print);
chunker.finish(print);                                                // reports the last chunk
```

For a buffer that is already in memory (for example a memory-mapped checkpoint), all cores, same chunks as above:

```cpp
const std::vector<cdc::Chunk> chunks = cdc::chunk_parallel("gear", cdc::Params::from_average(8192), data, size);
```

[examples/stream.cpp](examples/stream.cpp) is a complete program. Build and try the tools:

```bash
cmake -S . -B build && cmake --build build -j
ctest --test-dir build                       # unit tests and the cross-check against the reference

./build/cdc stats --algo gear --avg 8192 file.bin
# algo=gear bytes=16777216 chunks=1768
# mean=9489 stddev=2818 cv=0.30 min=2050 p5=4231 p50=9306 p95=14036 max=26066 forced_at_max=0

./build/cdc chunk --algo rabin --avg 8192 file.bin        # offset, length, hash per chunk
./build/cdc chunk --algo gear --threads 0 file.bin        # all cores, identical output
./build/cdc dedup --algo gear --avg 8192 old.bin new.bin  # how much of new.bin is already in old.bin
```

`Params` takes `min_size`, `avg_size` (a power of two), `max_size` and, for Gear, a `normalization` level (0-4).
Invalid limits throw `std::invalid_argument`.

## How it is tested

| Check | What it establishes |
| --- | --- |
| 31 unit tests | chunks tile the input and respect the limits; boundaries do not depend on how input is fed (1-byte to 70 KB pieces); rolling hashes equal the hash computed from the window; boundaries survive insertions and deletions; sizes match the target; the decision exactly at `min_size` matches the definition; 300 random parameter sets, contents and feed sizes satisfy the same invariants; multi-threaded chunks equal sequential chunks on 500 random cases, across 1 to 12 threads, and at every segment boundary offset |
| Differential test | the library's chunk lengths, single- and multi-threaded, equal those of `tests/reference.py` on 11 inputs and parameter sets. The reference computes every cut from the definition with no rolling state and shares no code with the library. It also checks that the Rabin polynomial is irreducible |
| Golden vectors | the boundaries of a fixed input do not change, which would invalidate stored chunk indexes |
| Mutation check | `tests/mutation_check.py` injects 23 bugs (off-by-one, swapped masks, wrong shift, wrong warm-up at a thread boundary, ...) and confirms the tests catch each; two edits that cannot matter, because the sliding window makes the state irrelevant, survive as expected |
| CI | gcc and clang on Linux (x86-64), clang on macOS (arm64), all with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`; the cross-check runs on each, so chunk boundaries are identical across the two architectures; an AddressSanitizer and UBSan job; a ThreadSanitizer job; the mutation check |

## Status and limits

The streaming interface is sequential; `chunk_parallel` needs the whole buffer in memory. No SIMD. Tested on Linux (x86-64) and macOS (arm64), not on Windows. A chunker object holds
per-stream state, so use one per stream. The `cdc` tool reads whole files into memory. The API may change before 1.0.

Design notes, deviations from the papers, and limits: [docs/DESIGN.md](docs/DESIGN.md).

## Layout

```
include/cdc/cdc.hpp   public API: Params, Chunker, GearChunker, RabinChunker, FixedChunker
src/                  implementations (gear, rabin, parallel)
tools/cdc_cli.cpp     chunk, stats, dedup, info
bench/bench.cpp       throughput, multi-core scaling, size distribution, dedup after edits
examples/stream.cpp   streaming example
tests/                unit tests, reference implementation, cross-check, mutation check
```

## Provenance and licence

Written from the published algorithms: Gear hashing and normalized chunking from FastCDC (Xia et al., USENIX ATC 2016) and
Rabin fingerprinting from LBFS (Muthitacharoen et al., SOSP 2001). No third-party code; the Gear table is generated in
code, so chunk boundaries will not match other implementations. MIT licensed.
