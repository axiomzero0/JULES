#!/usr/bin/env python3
"""Compare two bench results.csv files (old baseline vs fresh run)."""
import csv, sys, math

def load(path):
    d = {}
    for r in csv.DictReader(open(path)):
        if r["verified"] == "ok":
            d[(r["kernel"], r["config"])] = (float(r["median_s"]), float(r["compile_s"]))
    return d

old_path = "/home/z/my-project/jules/bench/results/results_20260905_baseline.csv"
new_path = "/home/z/my-project/jules/bench/results/results.csv"
old, new = load(old_path), load(new_path)

kernels = ["fib", "tak", "primes", "mandel", "flops", "inthash",
           "vecsum", "vecmask"]
jules_cfgs = ["julesc-aot", "julesc-aot-O3", "julesc-jit-baseline", "julesc-jit-optimizing"]

print(f"{'kernel':<9}{'config':<23}{'old':>9}{'new':>9}{'delta':>9}   verdict")
print("-" * 72)
for k in kernels:
    for c in jules_cfgs:
        o = old.get((k, c))
        n = new.get((k, c))
        if not o or not n:
            print(f"{k:<9}{c:<23}  MISSING")
            continue
        d = (n[0] - o[0]) / o[0] * 100.0
        verdict = "faster" if d < -2 else ("slower" if d > 2 else "~same")
        print(f"{k:<9}{c:<23}{o[0]:>9.4f}{n[0]:>9.4f}{d:>+8.1f}%   {verdict}")

# ratios vs gcc-O3 (fresh, both files agree on gcc since gcc didn't change)
print("\n=== fresh ratios vs gcc -O3 ===")
gm = {"julesc-aot": [], "julesc-aot-O3": []}
for k in kernels:
    g3 = new[(k, "gcc-O3")][0]
    row = [k]
    for c in ["julesc-aot", "julesc-aot-O3", "gcc-O2", "clang-O3"]:
        v = new[(k, c)][0] / g3
        if c in gm: gm[c].append(v)
        row.append(f"x{v:.2f}")
    print(f"{row[0]:<9} jules-O2 {row[1]:>6}  jules-O3 {row[2]:>6}  gcc-O2 {row[3]:>6}  clang-O3 {row[4]:>6}")
geo = lambda xs: math.exp(sum(math.log(x) for x in xs) / len(xs))
print(f"\ngeomean julesc-aot(-O2) vs gcc-O3:  x{geo(gm['julesc-aot']):.3f}")
print(f"geomean julesc-aot-O3   vs gcc-O3:  x{geo(gm['julesc-aot-O3']):.3f}")

# compile time
print("\n=== compile time (median across kernels) ===")
for c in ["julesc-aot", "gcc-O3", "clang-O3"]:
    ts = [new[(k, c)][1] for k in kernels]
    ts.sort()
    print(f"{c:<12} median {ts[len(ts)//2]*1000:.0f} ms  range {ts[0]*1000:.0f}-{ts[-1]*1000:.0f} ms")
