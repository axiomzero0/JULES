# JULES Benchmark Suite

Runtime and compile-time comparison of julesc against GCC and LLVM/Clang on
six scalar kernels.

## Layout

- `kernels/` — six kernels, each as an algorithmically identical `.jules` +
  `.c` pair (same loop structure, same printed checksum).
- `results/results.csv` — raw measurements (CPU-time median/min, wall median,
  compile time, reps, verification status).
- `results/summary.md` — auto-generated summary tables.
- `results/fig_runtime.png`, `results/fig_compile.png` — report figures.
- `results/cover.html` + `results/body.pdf` — PDF report sources.
- Harness: `scripts/jules_bench.py` (build/verify/time),
  `scripts/jules_bench_charts.py` (figures),
  `scripts/jules_bench_report.py` (ReportLab body).

## Run

```bash
./build.sh                                  # build julesc first
python3 bench/scripts/jules_bench.py        # or --kernels fib,tak,... / --reps 7
```

Clang 19 is resolved via `JULES_BENCH_CLANG` (env), then `PATH`, then the
legacy local extraction
`/home/z/my-project/tmp/clang-root/usr/lib/llvm-19/bin/clang`
(installed via `apt-get download` + `dpkg -x`, no root needed); edit `CLANG`
in the harness if that path is gone.

## Method notes

- Primary metric is per-process **CPU time** (getrusage of children): this
  container's cgroup CPU quota quantizes wall time into ~50 ms steps, which
  corrupts wall-clock comparisons. Wall medians are kept in the CSV as a
  secondary column.
- Outputs are verified byte-identical against a gcc -O2 reference before any
  timing; a mismatch marks the row WRONG instead of timing it.
- Runs are pinned to one core (`taskset -c 0`), warmup + adaptive reps,
  medians reported.

## Headline results (2026-09-04, gcc 14.2.0 / clang 19.1.7)

| julesc-aot (-O2) vs | geometric mean |
|---|---|
| gcc -O3 | 1.90x |
| clang -O3 | 1.74x |
| gcc -O0 | 0.81x (faster) |

Per-kernel vs gcc -O3: primes **1.00x (parity)**, inthash 1.22x, flops
**1.39x** (was 2.69x; loop rotation + in-place accumulator fusion + a
single taken branch per iteration — the loop is now the same 9-instruction
do-while shape gcc emits), mandel 2.44x (short-circuit conditions are
flattened to `and`/`or` with one branch, and the loop is rotated; the
remaining gap is bool materialization — setcc/movzx/mov/and against gcc's
direct-flags `comisd; jb` — plus the FP dependency chain), tak 3.36x, fib
3.41x. Compile-time geometric mean: julesc 21 ms vs gcc -O3 48 ms vs
clang -O3 69 ms.

History: 4.53x (first full run) -> 2.79x (opt levels + RA) -> 2.14x
(two-operand fusion/coalescing) -> 1.90x (loop rotation, pure-short-circuit
predication, fixpoint operand folds, single-use forwarding).

Remaining gap analysis (from disassembly): fib/tak are bound by the
non-inlined recursive call protocol (gcc keeps args in registers across the
whole frame and 32-bit encodings); flops' residual is the a-recurrence's
mul+add latency (gcc -O3 without -march has the same 8-cycle chain; its
remaining edge is one fewer loop-carried mov); mandel needs direct-flags
compare-and-branch for `&&` (setcc-free guard) and FP dependency-chain
breaking; no live-range splitting yet: spilling degenerates to memory
operands for that value.
