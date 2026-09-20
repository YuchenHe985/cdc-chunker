#!/usr/bin/env python3
"""Compares the C++ chunkers with the definition-based reference in reference.py.

For each case it writes deterministic input, runs `cdc chunk`, and requires the chunk lengths to match
exactly. It also checks that the constants the library reports match the reference and that the
Rabin polynomial is irreducible.
"""
import argparse
import os
import subprocess
import sys
import tempfile

import reference as ref

# (algorithm, min, avg, max, normalization, input size, seed, kind)
CASES = [
    ("gear", 256, 1024, 8192, 2, 96_000, 1, "random"),
    ("gear", 128, 512, 4096, 0, 96_000, 2, "random"),
    ("gear", 64, 128, 1024, 1, 48_000, 3, "random"),
    ("gear", 300, 2048, 16384, 4, 96_000, 4, "random"),
    ("gear", 256, 1024, 8192, 2, 90_000, 5, "repeats"),
    ("rabin", 256, 1024, 8192, 0, 96_000, 6, "random"),
    ("rabin", 64, 128, 1024, 0, 48_000, 7, "random"),
    ("rabin", 500, 4096, 32768, 0, 160_000, 8, "random"),
    ("rabin", 256, 1024, 8192, 0, 90_000, 9, "repeats"),
    # max_size close to avg_size: most chunks end at the forced cut
    ("gear", 64, 128, 192, 1, 48_000, 10, "random"),
    ("rabin", 64, 128, 192, 0, 48_000, 11, "random"),
]


def make_input(size, seed, kind):
    if kind == "repeats":  # a block repeated several times, then fresh data: boundaries must repeat too
        block = ref.random_bytes(size // 6, seed)
        return block * 4 + ref.random_bytes(size - 4 * len(block), seed + 100)
    return ref.random_bytes(size, seed)


def run_cli(cli, algo, mn, avg, mx, norm, path, extra=()):
    cmd = [cli, "chunk", "--algo", algo, "--avg", str(avg), "--min", str(mn), "--max", str(mx), "--norm", str(norm), *extra, path]
    out = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
    return [int(line.split("\t")[1]) for line in out.splitlines()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True, help="path to the cdc executable")
    args = ap.parse_args()
    failures = 0

    info = dict(line.split("=") for line in subprocess.run([args.cli, "info"], check=True, capture_output=True, text=True).stdout.split())
    expected = {
        "rabin_poly": hex(ref.RABIN_POLY).upper().replace("0X", "0x"),
        "rabin_window": str(ref.RABIN_WINDOW),
        "gear_seed": hex(ref.GEAR_SEED).upper().replace("0X", "0x"),
        "gear_window": str(ref.GEAR_WINDOW),
    }
    for key, want in expected.items():
        if info.get(key) != want:
            print(f"FAIL constant {key}: library has {info.get(key)}, reference has {want}")
            failures += 1
    if not ref.is_irreducible(ref.RABIN_POLY):
        print("FAIL the Rabin polynomial is not irreducible")
        failures += 1

    with tempfile.TemporaryDirectory() as tmp:
        for algo, mn, avg, mx, norm, size, seed, kind in CASES:
            data = make_input(size, seed, kind)
            path = os.path.join(tmp, "input.bin")
            with open(path, "wb") as f:
                f.write(data)
            if algo == "gear":
                want = ref.gear_boundaries(data, mn, avg, mx, norm)
            else:
                want = ref.rabin_boundaries(data, mn, avg, mx)
            # the default single thread, and the multi-threaded path with segments small enough to split
            # every input several times
            for mode, extra in (("1 thread", ()), ("4 threads", ("--threads", "4", "--segment-min", "2048"))):
                got = run_cli(args.cli, algo, mn, avg, mx, norm, path, extra)
                label = f"{algo} min={mn} avg={avg} max={mx} norm={norm} {kind} {size}B, {mode}"
                if got == want:
                    print(f"ok   {label}: {len(got)} chunks")
                else:
                    failures += 1
                    first = next((i for i, (a, b) in enumerate(zip(got, want)) if a != b), min(len(got), len(want)))
                    print(f"FAIL {label}: first difference at chunk {first}: library {got[first:first + 3]} reference {want[first:first + 3]}")
    print("cross-check:", "PASS" if failures == 0 else f"{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
