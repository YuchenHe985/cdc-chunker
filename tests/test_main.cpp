#include <cstdio>
#include <cstring>

#include "minitest.hpp"

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0, failed = 0;
  for (const auto& c : minitest::cases()) {
    if (filter != nullptr && std::strstr(c.name, filter) == nullptr) continue;
    const int before = minitest::failures();
    c.fn();
    const bool ok = minitest::failures() == before;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", c.name);
    ++ran;
    if (!ok) ++failed;
  }
  std::printf("%d tests, %d failed\n", ran, failed);
  return failed == 0 ? 0 : 1;
}
