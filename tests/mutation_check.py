#!/usr/bin/env python3
"""Mutation check: breaks the library on purpose and confirms the tests notice.

Each mutant is one small edit to the source (an off-by-one, a swapped mask, a wrong shift, ...). The
script builds the mutated copy, runs the unit tests and then the cross-check against the reference,
and reports which of them caught it. A mutant that survives means a behaviour is not tested.

"Equivalent" mutants are edits that cannot change the output by design (state that the sliding
window makes irrelevant); they are expected to survive, which documents that property.

    python3 tests/mutation_check.py            # all mutants (about 4 minutes)
    python3 tests/mutation_check.py "one byte" # only mutants whose name contains the text
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (name, file, old text, new text, expected outcome)
MUTANTS = [
    ("gear: hash window one byte short", "src/gear.cpp",
     "warm_start_ = p_.min_size - detail::kGearWindow;", "warm_start_ = p_.min_size - detail::kGearWindow + 1;", "kill"),
    ("gear: strict and relaxed masks swapped", "src/gear.cpp",
     "mask_s_ = top_mask(bits + p_.normalization);\n  mask_l_ = top_mask(bits - p_.normalization);",
     "mask_s_ = top_mask(bits - p_.normalization);\n  mask_l_ = top_mask(bits + p_.normalization);", "kill"),
    ("gear: strict region ends one byte early", "src/gear.cpp",
     "} else if (len_ + 1 < p_.avg_size) {", "} else if (len_ + 1 < p_.avg_size - 1) {", "kill"),
    ("gear: chunks may exceed max_size by one", "src/gear.cpp",
     "std::size_t stop = p_.max_size;", "std::size_t stop = p_.max_size + 1;", "kill"),
    ("gear: hash-only region mixes with xor instead of add", "src/gear.cpp",
     "for (; k < n; ++k) fp = (fp << 1) + g[p[k]];", "for (; k < n; ++k) fp = (fp << 1) ^ g[p[k]];", "kill"),
    ("gear: skips one byte too many before hashing", "src/gear.cpp",
     "std::min(size - i, warm_start_ - len_);", "std::min(size - i, warm_start_ - len_ + 1);", "kill"),
    ("gear: cut when the masked bits are non-zero", "src/gear.cpp",
     "if ((fp & mask) == 0) {", "if ((fp & mask) != 0) {", "kill"),
    ("gear: chunk offsets do not advance", "src/gear.cpp",
     "offset_ += len_;", "offset_ += 0;", "kill"),
    ("gear: final chunk is never reported", "src/gear.cpp",
     "if (len_ > 0) emit(sink);", "if (len_ > 0) { reset(); }", "kill"),
    ("rabin: mask has one bit too many", "src/rabin.cpp",
     "mask_ = p_.avg_size - 1;", "mask_ = p_.avg_size;", "kill"),
    ("rabin: hash window one byte short", "src/rabin.cpp",
     "warm_start_ = p_.min_size - detail::kRabinWindow;", "warm_start_ = p_.min_size - detail::kRabinWindow + 1;", "kill"),
    ("rabin: first cut allowed one byte after min_size", "src/rabin.cpp",
     "const bool check = len_ + 1 >= p_.min_size;", "const bool check = len_ >= p_.min_size;", "kill"),
    ("rabin: departing byte is not removed from the digest", "include/cdc/cdc.hpp",
     "digest ^= t.out[out];", "(void)out;", "kill"),
    ("rabin: ring buffer one slot short", "include/cdc/cdc.hpp",
     "if (++pos == kRabinWindow) pos = 0;", "if (++pos == kRabinWindow - 1) pos = 0;", "kill"),
    ("rabin: wrong reduction shift", "src/common.cpp",
     "t.shift = static_cast<unsigned>(k - 8);", "t.shift = static_cast<unsigned>(k - 7);", "kill"),
    ("gear: window definition shifted", "src/common.cpp",
     "h += g[w[n - 1 - j]] << j;", "h += g[w[n - 1 - j]] << (j + 1);", "kill"),
    ("fixed: block closes one byte early", "src/common.cpp",
     "if (len_ == size_) {", "if (len_ + 1 == size_) {", "kill"),
    ("fixed: leftover length is forgotten between feed() calls", "src/common.cpp",
     "len_ += take;", "len_ = take;", "kill"),
    ("equivalent: gear hash not cleared at a cut", "src/gear.cpp",
     "len_ = 0;\n  fp_ = 0;\n}", "len_ = 0;\n}", "equivalent"),
    ("equivalent: rabin window not cleared at a cut", "src/rabin.cpp",
     "len_ = 0;\n  roller_.reset();\n}", "len_ = 0;\n}", "equivalent"),
]


def run(cmd, cwd, timeout):
    try:
        p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout)
        return p.returncode
    except subprocess.TimeoutExpired:
        return "timeout"


def main():
    work = tempfile.mkdtemp(prefix="cdc-mutation-")
    try:
        for entry in ("include", "src", "tools", "bench", "examples", "tests", "CMakeLists.txt"):
            src = os.path.join(ROOT, entry)
            dst = os.path.join(work, entry)
            shutil.copytree(src, dst, ignore=shutil.ignore_patterns("__pycache__")) if os.path.isdir(src) else shutil.copy(src, dst)
        build = os.path.join(work, "build")
        if run(["cmake", "-S", work, "-B", build, "-DCMAKE_BUILD_TYPE=Release"], work, 300) != 0:
            print("cmake configure failed")
            return 2
        targets = ["--target", "cdc_tests", "cdc_cli"]

        def build_and_test():
            if run(["cmake", "--build", build, "-j"] + targets, work, 600) != 0:
                return "does not compile", None
            rc = run([os.path.join(build, "cdc_tests")], work, 60)
            if rc != 0:
                return "unit tests" if rc != "timeout" else "unit tests (timeout)", True
            rc = run([sys.executable, os.path.join(work, "tests", "cross_check.py"), "--cli", os.path.join(build, "cdc")], work, 300)
            if rc != 0:
                return "cross-check" if rc != "timeout" else "cross-check (timeout)", True
            return "-", False

        status, _ = build_and_test()
        if status != "-":
            print("the unmodified tree does not pass:", status)
            return 2

        selected = [m for m in MUTANTS if not sys.argv[1:] or any(a in m[0] for a in sys.argv[1:])]
        bad = 0
        print(f"{'mutant':<62} {'caught by':<22} result")
        for name, rel, old, new, expect in selected:
            path = os.path.join(work, rel)
            original = open(path).read()
            if original.count(old) != 1:
                print(f"{name:<62} {'?':<22} INVALID (pattern found {original.count(old)} times)")
                bad += 1
                continue
            open(path, "w").write(original.replace(old, new))
            try:
                caught_by, caught = build_and_test()
            finally:
                open(path, "w").write(original)
            if caught_by == "does not compile":
                result, bad = "INVALID (does not compile)", bad + 1
            elif expect == "kill":
                result = "killed" if caught else "SURVIVED"
                bad += 0 if caught else 1
            else:
                result = "survived, as expected" if not caught else "KILLED (not equivalent)"
                bad += 1 if caught else 0
            print(f"{name:<62} {caught_by:<22} {result}")
        kills = sum(1 for m in selected if m[4] == "kill")
        print(f"\n{kills} mutants expected to be killed, {len(selected) - kills} equivalent; problems: {bad}")
        return 1 if bad else 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
