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
| gcc -O3 | 1.48x |
| clang -O3 | 1.38x |
| gcc -O0 | 0.67x (faster) |

Per-kernel vs gcc -O3: primes **1.00x (parity)**, flops **1.02x**, mandel
**1.19x** (was 2.44x; the `&&` guard now lowers as fused compare-and-branch
pairs straight off the flags — the exact `cmp; jle` + `ucomisd; jbe` shape
gcc emits, zero setcc/movzx/and round trips — with the loop rotated to
do-while form and the guard's x*x+y*y value kept in a register across the
branches), inthash 1.22x (lea-formed IV updates, rotated, direct
compare-immediate bounds), fib **2.15x** (was 3.41x; pass 50 now performs
recursion unrolling via accumulator introduction — the transform gcc
applies — halving the dynamic call count), tak 3.31x. Compile-time
geometric mean: julesc 21 ms vs gcc -O3 ~50 ms vs clang -O3 68 ms.

History: 4.53x (first full run) -> 2.79x (opt levels + RA) -> 2.14x
(two-operand fusion/coalescing) -> 1.90x (loop rotation, pure-short-circuit
predication, fixpoint operand folds, single-use forwarding) -> 1.48x (fused
short-circuit branches off compare flags, Form-B loop rotation, full-width
FP moves, compare-immediate folding, lea formation, dead-function
elimination after inlining, accumulator recursion unrolling).

Remaining gap analysis (from disassembly): tak is bound by the recursive
call protocol (nine argument-setup moves per call against gcc's
partial-application register placement; argument-register coalescing at
call sites is the identified lever). mandel/inthash carry one register move
per unrolled pair — the allocator's coalescing hints are pairwise, so
two-step phi cycles through unrolled copies keep one copy where gcc keeps
zero (component-based coalescing is the fix). primes trails only clang's
vectorized sieve (1.62x) — the SIMD passes remain documented scaffolds.
Two correctness holes found and fixed this round: compares against
non-imm32-encodable i64 constants emitted un-assemblable `cmpq $big, reg`
(t21), and the inliner's call-result rewrite confused value-slot and
memory-slot users — a `Cast(call)`'s operand landed on the callee's exit
allocation and printed a heap pointer (fixed with slot-kind-aware
rewiring; 12 lifetime miscompiles total, all regression-locked).
