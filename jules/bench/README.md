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

## Headline results (2026-09-05, gcc 14.2.0 / clang 19.1.7)

| config vs | geometric mean |
|---|---|
| julesc-aot -O2 vs gcc -O3 | 1.35x |
| julesc-aot -O2 vs clang -O3 | 1.23x |
| julesc-aot -O2 vs gcc -O0 | 0.60x (faster) |
| julesc-aot -O3 vs gcc -O3 | 1.25x |
| julesc-aot -O3 vs clang -O3 | 1.14x |

Per-kernel vs gcc -O3: primes **1.00x (parity)**, flops **1.01x**, mandel
**1.21x** (the `&&` guard lowers as fused compare-and-branch pairs straight
off the flags, loop rotated to do-while, guard value kept live across the
branches), inthash 1.22x (lea-formed IV updates, rotated, direct
compare-immediate bounds), fib **2.10x** (pass 50's accumulator
introduction halves the dynamic call count; the remaining gap is the
loop-carried call feeding the acc phi, which the inliner cannot yet
inline — the documented next lever), tak **1.23x** (was 3.31x: pass 50 now
converts the self tail call into a REAL phi-threaded loop — the old
jump-to-entry TCO tore down and rebuilt the frame every spine iteration —
and at -O3 inlines the recursion one level into itself (the clone's calls
target the expanded function, so every physical call executes two logical
levels: 118M -> 57M calls); pass 88's entry-fallthrough layout makes the
leaf path zero-taken-jump, and pass 87 retargets `lea -1(%rbx),%rdi`
straight into the argument register). tak now beats clang -O3 by 1.5x.
Compile-time geometric mean: julesc 21-24 ms vs gcc -O3 ~50 ms vs clang
-O3 68 ms.

History: 4.53x (first full run) -> 2.79x (opt levels + RA) -> 2.14x
(two-operand fusion/coalescing) -> 1.90x (loop rotation, pure-short-circuit
predication, fixpoint operand folds, single-use forwarding) -> 1.48x (fused
short-circuit branches off compare flags, Form-B loop rotation, full-width
FP moves, compare-immediate folding, lea formation, dead-function
elimination after inlining, accumulator recursion unrolling) -> 1.25x
(tail-call-to-loop conversion, recursive self-inlining at -O3,
loop-entry fallthrough layout, producer destination retarget, commutative
phi-backedge fusion).

Remaining gap analysis (from disassembly): fib is bound by the
accumulator loop's one call per level — the call feeds the acc phi's
backedge and the inliner's repin closure does not support calls inside
phi cycles, so self-inlining cannot expand it yet (lifting that
limitation is the identified lever). mandel/inthash carry one register
move per unrolled pair — the allocator's coalescing hints are pairwise, so
two-step phi cycles through unrolled copies keep one copy where gcc keeps
zero (component-based coalescing is the fix). primes trails only clang's
vectorized sieve (1.62x) — the SIMD passes remain documented scaffolds.
Two correctness holes found and fixed this round: compares against
non-imm32-encodable i64 constants emitted un-assemblable `cmpq $big, reg`
(t21), and the inliner's call-result rewrite confused value-slot and
memory-slot users — a `Cast(call)`'s operand landed on the callee's exit
allocation and printed a heap pointer (fixed with slot-kind-aware
rewiring; 12 lifetime miscompiles total, all regression-locked).
