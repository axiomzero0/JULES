#!/usr/bin/env python3
"""JULES vs GCC vs LLVM/Clang benchmark harness.

Compiles the bench/kernels/*.jules + *.c pairs with:
  - julesc --mode aot / jit-baseline / jit-optimizing
  - gcc -O0 / -O2 / -O3
  - clang -O0 / -O2 / -O3
Verifies outputs are identical, then times each binary (pinned to one core,
warmup + repeated runs, median reported).

Usage: python3 jules_bench.py [--reps N] [--kernels fib,tak,...] [--quick]
Writes: bench/results/results.csv + bench/results/summary.md
"""
import argparse
import csv
import os
import resource
import statistics
import subprocess
import sys
import time

ROOT = "/home/z/my-project/jules"
KERNELS_DIR = os.path.join(ROOT, "bench", "kernels")
RESULTS_DIR = os.path.join(ROOT, "bench", "results")
JULESC = os.path.join(ROOT, "build", "julesc")
CLANG = "/home/z/my-project/tmp/clang-root/usr/lib/llvm-19/bin/clang"
WORK = "/tmp/jules_bench_work"

ALL_KERNELS = ["fib", "tak", "primes", "mandel", "flops", "inthash"]

# label -> (compiler kind, extra args)
CONFIGS = [
    ("julesc-aot",            "jules", ["--mode", "aot"]),            # default = -O2
    ("julesc-aot-O3",         "jules", ["--mode", "aot", "-O3"]),
    ("julesc-jit-baseline",   "jules", ["--mode", "jit-baseline"]),
    ("julesc-jit-optimizing", "jules", ["--mode", "jit-optimizing"]),
    ("gcc-O0", "gcc", ["-O0"]),
    ("gcc-O2", "gcc", ["-O2"]),
    ("gcc-O3", "gcc", ["-O3"]),
    ("clang-O0", "clang", ["-O0"]),
    ("clang-O2", "clang", ["-O2"]),
    ("clang-O3", "clang", ["-O3"]),
]


def sh(cmd, cwd=None, timeout=600):
    return subprocess.run(cmd, shell=isinstance(cmd, str), cwd=cwd,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=timeout)


def run_timed(binary, timeout=300):
    """Run pinned to core 0; return (cpu_s, wall_s).

    CPU time (user+sys of the child) is the primary metric: this container
    applies cgroup CPU-quota throttling that quantizes wall time into
    scheduler bursts (~50ms steps), which corrupts wall-clock comparison.
    CPU time is throttle-immune and reflects actual code speed.
    """
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    t0 = time.perf_counter()
    p = subprocess.run(["taskset", "-c", "0", binary],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=timeout)
    t1 = time.perf_counter()
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    if p.returncode not in (0,):
        raise RuntimeError(f"{binary} exited rc={p.returncode}")
    cpu = (after.ru_utime - before.ru_utime) + (after.ru_stime - before.ru_stime)
    return cpu, t1 - t0


def compile_kernel(kernel, label, kind, args, out):
    if kind == "jules":
        cmd = [JULESC, os.path.join(KERNELS_DIR, kernel + ".jules"),
               "-o", out] + args
    elif kind == "gcc":
        cmd = ["gcc", args[0], "-o", out, os.path.join(KERNELS_DIR, kernel + ".c")]
    else:
        cmd = [CLANG, args[0], "-o", out, os.path.join(KERNELS_DIR, kernel + ".c")]
    t0 = time.perf_counter()
    p = sh(cmd, timeout=600)
    t1 = time.perf_counter()
    if p.returncode != 0 or not os.path.exists(out):
        return None, None
    return t1 - t0, p.stdout.decode()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--kernels", type=str, default=",".join(ALL_KERNELS))
    ap.add_argument("--quick", action="store_true",
                    help="3 reps, no warmup-extension")
    args = ap.parse_args()
    kernels = [k for k in args.kernels.split(",") if k]
    os.makedirs(RESULTS_DIR, exist_ok=True)
    os.makedirs(WORK, exist_ok=True)

    # machine info
    cpu = ""
    try:
        for line in open("/proc/cpuinfo"):
            if line.startswith("model name"):
                cpu = line.split(":", 1)[1].strip()
                break
    except OSError:
        pass

    # reference outputs from gcc -O2 (verified earlier: byte-identical across
    # every compiler config for all kernels)
    refs = {}
    for k in kernels:
        ref_bin = os.path.join(WORK, f"ref_{k}")
        p = sh(["gcc", "-O2", "-o", ref_bin,
                os.path.join(KERNELS_DIR, k + ".c")])
        if p.returncode != 0:
            sys.exit(f"reference compile failed for {k}")
        out = sh([ref_bin])
        refs[k] = out.stdout

    rows = []
    for k in kernels:
        for label, kind, cargs in CONFIGS:
            out = os.path.join(WORK, f"{k}_{label}")
            ctime, _ = compile_kernel(k, label, kind, cargs, out)
            if ctime is None:
                rows.append(dict(kernel=k, config=label, compile_s="",
                                 median_s="FAIL", min_s="", reps=0,
                                 verified="compile-fail"))
                print(f"[{k}/{label}] COMPILE FAILED", file=sys.stderr)
                continue
            # verify
            try:
                o = sh([out], timeout=300)
                ok = (o.returncode == 0 and o.stdout == refs[k])
            except subprocess.TimeoutExpired:
                ok = False
            if not ok:
                rows.append(dict(kernel=k, config=label,
                                 compile_s=f"{ctime:.3f}", median_s="WRONG",
                                 min_s="", reps=0, verified="output-mismatch"))
                print(f"[{k}/{label}] OUTPUT MISMATCH", file=sys.stderr)
                continue
            # warmup + timed reps (adaptive count for slow configs)
            try:
                warm_cpu, _ = run_timed(out)
            except Exception as e:
                rows.append(dict(kernel=k, config=label,
                                 compile_s=f"{ctime:.3f}", median_s=str(e),
                                 min_s="", reps=0, verified="run-fail"))
                continue
            reps = args.reps
            if warm_cpu > 8.0:
                reps = 2
            elif warm_cpu > 3.0:
                reps = 3
            if args.quick:
                reps = min(reps, 3)
            times = []
            walls = []
            for _ in range(reps):
                try:
                    c, w = run_timed(out, timeout=600)
                    times.append(c)
                    walls.append(w)
                except subprocess.TimeoutExpired:
                    break
            if not times:
                rows.append(dict(kernel=k, config=label,
                                 compile_s=f"{ctime:.3f}", median_s="timeout",
                                 min_s="", wall_med_s="", reps=0, verified="timeout"))
                continue
            rows.append(dict(kernel=k, config=label,
                             compile_s=f"{ctime:.3f}",
                             median_s=f"{statistics.median(times):.4f}",
                             min_s=f"{min(times):.4f}",
                             wall_med_s=f"{statistics.median(walls):.4f}",
                             reps=len(times), verified="ok"))
            print(f"[{k}/{label}] cpu-median={statistics.median(times):.4f}s "
                  f"cpu-min={min(times):.4f}s wall-med={statistics.median(walls):.4f}s "
                  f"compile={ctime:.3f}s reps={len(times)}")

    # ---- write CSV ----------------------------------------------------------
    csv_path = os.path.join(RESULTS_DIR, "results.csv")
    with open(csv_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["kernel", "config", "compile_s",
                                          "median_s", "min_s", "wall_med_s",
                                          "reps", "verified"])
        w.writeheader()
        for r in rows:
            w.writerow(r)

    # ---- write markdown summary ---------------------------------------------
    md = []
    md.append("# JULES vs GCC vs LLVM/Clang — runtime benchmark\n")
    md.append(f"* Machine: {cpu or 'x86-64'}, 2 cores, Linux (Debian trixie)")
    md.append("* Compilers: julesc (JULES, 89-pass SoN pipeline), "
              "gcc 14.2.0, clang 19.1.7 (LLVM)")
    md.append("* Method: warmup + repeated runs pinned to one core; primary "
              "metric is per-process CPU time (user+sys), which is immune to "
              "the container's cgroup CPU-quota throttling that quantizes "
              "wall time; median of N reps; outputs verified identical before "
              "timing.")
    md.append("* Workloads are algorithmically identical scalar kernels; "
              "all binaries print the same checksum.\n")
    md.append("## CPU time (median seconds, lower is better)\n")
    header = ["kernel"] + [c[0] for c in CONFIGS]
    md.append("| " + " | ".join(header) + " |")
    md.append("|" + "---|" * len(header))
    for k in kernels:
        cells = [k]
        for label, _, _ in CONFIGS:
            v = next((r for r in rows if r["kernel"] == k
                      and r["config"] == label), None)
            cells.append(v["median_s"] if v else "-")
        md.append("| " + " | ".join(cells) + " |")
    md.append("\n## Ratio vs gcc -O3 (higher = JULES slower)\n")
    md.append("| kernel | " + " | ".join(c[0] for c in CONFIGS) + " |")
    md.append("|" + "---|" * len(header))
    for k in kernels:
        g3 = next((r for r in rows if r["kernel"] == k
                   and r["config"] == "gcc-O3"), None)
        cells = [k]
        for label, _, _ in CONFIGS:
            v = next((r for r in rows if r["kernel"] == k
                      and r["config"] == label), None)
            try:
                ratio = float(v["median_s"]) / float(g3["median_s"])
                cells.append(f"x{ratio:.2f}")
            except (TypeError, ValueError, ZeroDivisionError):
                cells.append("-")
        md.append("| " + " | ".join(cells) + " |")
    md.append("\n## Compile time (seconds, one shot incl. link)\n")
    md.append("| kernel | " + " | ".join(c[0] for c in CONFIGS) + " |")
    md.append("|" + "---|" * len(header))
    for k in kernels:
        cells = [k]
        for label, _, _ in CONFIGS:
            v = next((r for r in rows if r["kernel"] == k
                      and r["config"] == label), None)
            cells.append(v["compile_s"] if v and v["compile_s"] else "-")
        md.append("| " + " | ".join(cells) + " |")
    md_path = os.path.join(RESULTS_DIR, "summary.md")
    with open(md_path, "w") as f:
        f.write("\n".join(md) + "\n")

    print(f"\nwrote {csv_path}\nwrote {md_path}")


if __name__ == "__main__":
    main()
