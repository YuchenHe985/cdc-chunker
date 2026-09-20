#!/usr/bin/env python3
"""How much of a model file is unchanged, tensor group by tensor group.

Chunk-level deduplication (see `cdc dedup`) can only reuse a chunk when every byte in it is identical, so what matters is which
tensors are bit-for-bit unchanged between two versions of a model. This reads the headers of two .safetensors files, compares the
raw bytes of every tensor with the same name, and reports the share of identical elements per group of tensors (layer numbers are
folded, so `model.layers.7.mlp.up_proj.weight` and `model.layers.8.mlp.up_proj.weight` are one group).

    python3 tools/compare_tensors.py base/model.safetensors finetuned/model.safetensors

Both files should use the same dtype: a BF16 file and an F16 file of the same weights differ in every byte. Needs numpy only.
"""
import json
import re
import struct
import sys

import numpy as np

ELEMENT_BYTES = {"F64": 8, "F32": 4, "F16": 2, "BF16": 2, "I64": 8, "I32": 4, "I16": 2, "I8": 1, "U8": 1, "BOOL": 1}
UINT = {1: np.uint8, 2: np.uint16, 4: np.uint32, 8: np.uint64}


def read_header(path):
    with open(path, "rb") as f:
        (n,) = struct.unpack("<Q", f.read(8))
        header = json.loads(f.read(n))
    header.pop("__metadata__", None)
    return header, 8 + n


def group_of(name):
    return re.sub(r"\.\d+\.", ".N.", name)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    old_path, new_path = sys.argv[1:]
    old_header, old_base = read_header(old_path)
    new_header, new_base = read_header(new_path)
    old = np.memmap(old_path, dtype=np.uint8, mode="r")
    new = np.memmap(new_path, dtype=np.uint8, mode="r")
    groups = {}
    only_in_one = set(old_header) ^ set(new_header)
    for name, meta in new_header.items():
        if name not in old_header:
            continue
        other = old_header[name]
        if other["dtype"] != meta["dtype"] or other["shape"] != meta["shape"]:
            sys.exit(f"{name}: dtype/shape differ ({other['dtype']} {other['shape']} vs {meta['dtype']} {meta['shape']}); convert first")
        size = ELEMENT_BYTES[meta["dtype"]]
        a0, a1 = old_base + other["data_offsets"][0], old_base + other["data_offsets"][1]
        b0, b1 = new_base + meta["data_offsets"][0], new_base + meta["data_offsets"][1]
        a = np.frombuffer(old[a0:a1], dtype=UINT[size])
        b = np.frombuffer(new[b0:b1], dtype=UINT[size])
        g = groups.setdefault(group_of(name), [0, 0, 0])
        g[0] += b1 - b0
        g[1] += int((a == b).sum())
        g[2] += a.size
    total_bytes = sum(g[0] for g in groups.values())
    print(f"{'tensor group':<44}{'MB':>9}{'share of file':>15}{'elements identical':>20}")
    for name, (nbytes, same, count) in sorted(groups.items(), key=lambda kv: -kv[1][0]):
        print(f"{name:<44}{nbytes / 1e6:>9.1f}{100 * nbytes / total_bytes:>14.1f}%{100 * same / count:>19.1f}%")
    same_bytes = sum(g[0] * g[1] / g[2] for g in groups.values())
    print(f"\nidentical elements, weighted by size: {100 * same_bytes / total_bytes:.1f}% of {total_bytes / 1e6:.1f} MB")
    if only_in_one:
        print(f"{len(only_in_one)} tensors exist in only one file (not counted)")


if __name__ == "__main__":
    main()
