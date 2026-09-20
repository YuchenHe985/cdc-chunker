// A minimal test harness so the project has no dependencies.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "cdc/cdc.hpp"

namespace minitest {

struct Case {
  const char* name;
  void (*fn)();
};

inline std::vector<Case>& cases() {
  static std::vector<Case> v;
  return v;
}

inline int& failures() {
  static int n = 0;
  return n;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { cases().push_back({name, fn}); }
};

inline void fail(const char* file, int line, const std::string& what) {
  ++failures();
  std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, what.c_str());
}

// Deterministic pseudo-random bytes (splitmix64), the same generator tests/reference.py uses.
inline std::vector<std::uint8_t> random_bytes(std::size_t n, std::uint64_t seed) {
  std::vector<std::uint8_t> out(n);
  std::uint64_t state = seed;
  for (std::size_t i = 0; i < n; i += 8) {
    std::uint64_t v = cdc::detail::splitmix64(state);
    for (std::size_t j = 0; j < 8 && i + j < n; ++j) out[i + j] = static_cast<std::uint8_t>(v >> (8 * j));
  }
  return out;
}

}  // namespace minitest

#define TEST(name)                                        \
  static void name();                                     \
  static minitest::Registrar registrar_##name(#name, name); \
  static void name()

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) minitest::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
  } while (0)

#define CHECK_EQ(a, b)                                                                          \
  do {                                                                                          \
    const auto va_ = (a);                                                                       \
    const auto vb_ = (b);                                                                       \
    if (!(va_ == vb_))                                                                          \
      minitest::fail(__FILE__, __LINE__, "CHECK_EQ(" #a ", " #b ") -> " + std::to_string(va_) + \
                                            " vs " + std::to_string(vb_));                      \
  } while (0)

#define CHECK_THROWS(expr)                                                   \
  do {                                                                       \
    bool threw_ = false;                                                     \
    try {                                                                    \
      (void)(expr);                                                          \
    } catch (const std::exception&) {                                        \
      threw_ = true;                                                         \
    }                                                                        \
    if (!threw_) minitest::fail(__FILE__, __LINE__, "expected exception: " #expr); \
  } while (0)
