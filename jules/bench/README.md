# JULES Benchmark Suite

Runtime and compile-time comparison of julesc against GCC and LLVM/Clang on
eight kernels: six scalar (fib, tak, primes, mandel, flops, inthash) plus
two vector-family kernels (vecsum: i64 array reduction; vecmask: f64
masked-blend element-wise) added by the 2026-09-22 vector-benchmark round
so the SIMD passes are measured, not just claimed.

## Layout

- `kernels/` — eight kernels, each as an algorithmically identical `.jules` +
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

## Headline results (2026-09-22, gcc 14.2.0 / clang 19.1.7, 8 kernels)

| config vs | geometric mean |
|---|---|
| julesc-aot -O2 vs gcc -O3 | **0.93x (faster)** |
| julesc-aot -O3 vs gcc -O3 | **0.90x (faster)** |
| julesc-aot -O2 vs gcc -O0 | ~0.60x (faster) |

Per-kernel vs gcc -O3: fib **0.17x (6x faster)** — spine-tail base
widening prunes the recursion tree (see the recursion round below),
primes **1.00x (parity)**, flops 1.01x, mandel 1.07x, vecmask 1.05x
(one `maxpd` per lane pair — the same instruction gcc emits), vecsum
1.11x (two-accumulator unrolled paddq loop; the residue is scalar
remainder + addressing), inthash 1.23x, tak 1.95x at -O2 / 1.52x at
-O3 (3-arg recursion — not the accumulator shape).

## The recursion round (2026-09-22, evening)

fib was 2.09x vs gcc -O3: the accumulator loop's one `call` per level
(the loop-carried call feeds the acc phi's backedge — the documented
inliner limitation). The disassembly of gcc showed it does NOT prune the
recursion tree (it visits every node with an unrolled loop); the actual
lever is different and bigger:

1. **Spine-tail base widening** (`p50`): when f(k) constant-folds for
   every k in [0..K] and the recursion is structurally descending
   (g2 = x - c, c >= 1) with a constant-bounded base (C = x <s d,
   d <= K+1), the accumulator loop exits at `w <= K` through the folded
   table `TBL[w] + acc` instead of running the spine down to C. The
   identity is exact: the loop's remaining contribution from spine value
   w IS f(w) (unroll the recursion definition and the call terms line up
   with the per-step calls one-to-one); the fold proves it bottoms out
   in constants. Every f(k <= K) subtree collapses to one constant —
   on fib(39) the executed-call count drops ~75x (C(39) -> C(30)), and
   fib lands at **0.17x gcc-O3** (gcc still walks all 2e8 tree nodes at
   ~1.6 cycles each; the folded table prunes them outright). The head
   stays a single signed compare; negative arguments take the original
   B(w) default arm (they are C-true by the structural gate — this is
   why the gates exist: naive widening breaks on ascending spines and
   on base bounds above the table).
2. **Vector-loop unroll x2** (`p56`): two packs per iteration (k counts
   32-byte pairs; nv = bound/(VF*2); an odd pack count tails into the
   scalar remainder via the floor-floor division identity). Integer
   Add reductions split into TWO accumulators (independent chains,
   merged lane-wise at the exit — exact for integers); FP/Min/Max/Mul
   keep one accumulator with the two updates chained. vecsum's loop:
   13 instructions per 4 elements (was 20), 0.046s -> 0.033s.
3. **Operand-copy elimination** (`p87`, now CFG-liveness-based): the
   pair-fold promotes a load's slot round-trip into a `movaps` copy,
   but a two-operand SSE op's operand register is fungible — the move
   is deleted and the op reads the load's register directly. The
   peephole gained a proper register-liveness pass over the CFG (labels
   + jump edges, u32 reg bitmasks) because the old linear dead-scan
   aborts at every loop backedge. vecsum: `movups; paddq` with no copy.
4. **Clamp-to-zero min/max** (`p61`): `Select(Gt(t,Z), t, Z)` (Z = the
   same +0.0 both arms broadcast) rewrites to a single `Max(Z, t)` —
   emitted as `maxpd dst=t src=Z`, bitwise-exact on every lane INCLUDING
   NaN and -0.0 (verified on hardware: unordered and equal-zero cases
   return SRC = +0.0, exactly the select's arms). The Ge/Le forms are
   deliberately REJECTED: the select yields the raw -0.0 where the
   instruction yields +0.0. vecmask: `cmpnlepd + pand` -> one `maxpd`,
   the same instruction sequence gcc emits for the same source.

Result: geomean vs gcc-O3 **1.34x -> 0.93x (O2) / 0.90x (O3)** — the
8-kernel suite is now faster than gcc -O3 overall; suite 313/313
(new t39_widenbase locks the widening's table-edge, negative-input and
rejected-shape cases; the min/max forms are locked by t38/t29 outputs,
which must stay bit-identical under the rewrite).

Open item: the JIT configs trail their own AOT (vecsum 0.041 vs 0.033,
vecmask 0.54 vs 0.18) — the JIT's pass schedule misses part of the
vector family's final shape; predates this round and unchanged by it.

## The vector-kernel round (2026-09-22)

First measurement of the SIMD family (before this round NO kernel touched
the vector passes — their state was unaudited by the suite): vecsum came
out **9.60x** and vecmask **2.45x** vs gcc -O3. Three fixes followed, all
found by disassembly, each verified against the byte-identical reference:

1. **RA packed-chain fusion** (`x64_ra.cpp`): the accumulator-chain fuse
   and the FP operand fold only knew `FpBin/FpNeg` — packed ops
   (`VecBin*/VecLogical/VecCmp*/VecBcast`, same xmm0-dst isel contract)
   never fused, leaving four `movaps` copies around every `paddq`.
   vecsum's loop went from 6 instructions per pair to 2
   (`movups + paddq %xmm3,%xmm4` in place).
2. **Degenerate mask blend** (`p61`): `Select(m, t, ZERO)` lowered as the
   full `Or(And(m,t), AndNot(m,f))` triple; with a zero arm the bitwise
   identities collapse it to a single `And`/`AndNot`. vecmask's blend
   chain: `cmpnlepd + pand` (the zero broadcast disappears entirely).
   gcc lowers the same source to one `maxpd` — a further idiom-match
   (packed Max) remains future work.
3. **Two real miscompile-class bugs in the RA's copy-elision family**,
   exposed by pinning the vectorizer's trip count at the loop entry
   (see below): the pair-fold and the single-use home-store forwarding
   both delete a value's slot round-trip and leave it living in the
   UNTRACKED isel scratch register (rax) across a loop body — where the
   address chains write rax unconditionally. Both now carry a
   backedge-straddle guard (store outside a loop + reload/consumer at or
   after its head = keep the round-trip). Symptom before the guard:
   `idiv`-computed trip count clobbered on iteration 2, segfault in
   t24/t32/t38 at -O2+.
4. **Loop-invariant guard recompute** (`p56`): the vector trip count
   `nv = bound/lanes` was pinned at the loop HEAD — an `idiv` executed
   EVERY vector iteration (~30 cycles per 2-element step; the whole
   9.6x gap started here). Now pinned at the entry edge, computed once.

Result: vecsum 0.283s -> 0.046s (6.1x), vecmask 0.403s -> 0.196s (2.1x);
suite 304/304 (the two RA guards are regression-locked by every existing
vector test; the straddle shapes appear throughout t23-t25/t32/t38).

History: 4.53x (first full run) -> 2.79x (opt levels + RA) -> 2.14x
(two-operand fusion/coalescing) -> 1.90x (loop rotation, pure-short-circuit
predication, fixpoint operand folds, single-use forwarding) -> 1.48x (fused
short-circuit branches off compare flags, Form-B loop rotation, full-width
FP moves, compare-immediate folding, lea formation, dead-function
elimination after inlining, accumulator recursion unrolling) -> 1.25x
(tail-call-to-loop conversion, recursive self-inlining at -O3,
loop-entry fallthrough layout, producer destination retarget, commutative
phi-backedge fusion) -> 1.34x on the 8-kernel suite (vector family joins
at 1.57x/1.22x after the round above; the 6 scalar kernels hold at 1.33x)
-> **0.93x (O2) / 0.90x (O3)** — base widening + vector unroll +
operand-copy elimination + clamp min/max (the recursion round).

Remaining gap analysis (from disassembly): tak is the last big scalar gap
(1.95x/-O2, 1.52x/-O3) — a 3-argument mutual recursion, not the
accumulator shape; the lever is multi-argument self-inlining. inthash
1.23x and mandel 1.07x carry one register move per unrolled pair — the
allocator's coalescing hints are pairwise, so two-step phi cycles
through unrolled copies keep one copy where gcc keeps zero
(component-based coalescing is the fix). vecsum 1.11x / vecmask 1.05x
residue: the scalar remainder loops recompute full addresses per
element (base + index each iteration) where gcc keeps a bumping
pointer with displacement addressing (mov r/base + add per element vs
lea disp(%reg)). vecmask's checksum loop is scalar in both compilers
(strict FP), but gcc unrolls its addsd 2x with negative-displacement
loads. Correctness holes found in earlier rounds (cmp-$big encoding,
inliner slot-kind rewiring, RA backedge-straddle copy elision) are
regression-locked in tests/.
