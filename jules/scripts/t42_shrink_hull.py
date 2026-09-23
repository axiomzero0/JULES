#!/usr/bin/env python3
"""t42 adversarial profile: shrink the sketch [min, max] hulls so the use
build's range guards FAIL on values the real profile covered.

Profile format: 8-byte header (u32 magic 'JPG1', u32 count), then `count`
little-endian u64 counters. Sketch j occupies counters [base + 5j, +5) as
[first, total, match, min, max]; base = 2 * pgo_loop_pairs.

The sketch is located by SIGNATURE (first=3, total=202, match=29 for
scale.c; then scale.x at +5) rather than a fixed base — robust to pass-43
loop instrumentation changing the base. Fails loudly if not found.
"""
import struct
import sys

src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()
magic, count = struct.unpack_from("<II", data, 0)
assert magic == 0x3150474A, f"bad magic {magic:#x}"
vals = list(struct.unpack_from(f"<{count}Q", data, 8))

# locate sketch c by signature, then sketch x follows at +5
base = None
for k in range(0, max(0, count - 4)):
    if vals[k] == 3 and vals[k + 1] == 202 and vals[k + 2] == 30:
        base = k
        break
assert base is not None, f"sketch signature not found in {count} counters"

# shrink hulls: c [3,9] -> [4,8];  x [0,199] -> [1,198]
vals[base + 3], vals[base + 4] = 4, 8
vals[base + 8], vals[base + 9] = 1, 198

open(dst, "wb").write(struct.pack("<II", magic, count))
open(dst, "ab").write(struct.pack(f"<{count}Q", *vals))
print(f"patched {count} counters: sketch@{base} c->[4,8], sketch@{base+5} x->[1,198]")
