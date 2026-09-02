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
- Harness: `/home/z/my-project/scripts/jules_bench.py` (build/verify/time),
  `/home/z/my-project/scripts/jules_bench_charts.py` (figures),
  `/home/z/my-project/scripts/jules_bench_report.py` (ReportLab body).

## Run

```bash
./build.sh                                   # build julesc first
python3 /home/z/my-project/scripts/jules_bench.py --reps 7
```

Clang 19 is expected at
`/home/z/my-project/tmp/clang-root/usr/lib/llvm-19/bin/clang`
(installed via `apt-get download` + `dpkg -x`, no root needed); reinstall it
or edit `CLANG` in the harness if that path is gone.

## Method notes

- Primary metric is per-process **CPU time** (getrusage of children): this
  container's cgroup CPU quota quantizes wall time into ~50 ms steps, which
  corrupts wall-clock comparisons. Wall medians are kept in the CSV as a
  secondary column.
- Outputs are verified byte-identical against a gcc -O2 reference before any
  timing; a mismatch marks the row WRONG instead of timing it.
- Runs are pinned to one core (`taskset -c 0`), warmup + adaptive reps,
  medians reported.

## Headline results (2026-09-02, gcc 14.2.0 / clang 19.1.7)

| julesc-aot vs | geometric mean |
|---|---|
| gcc -O3 | 2.79x |
| clang -O3 | 2.56x |
| gcc -O0 | 1.23x |

Compile-time geometric mean: julesc 21 ms vs gcc -O3 51 ms vs clang -O3 68 ms.
Kernel-level detail: see `results/summary.md` and the PDF report.
Configurations now include `julesc-aot-O3`; the default `julesc-aot` row
compiles at the -O2 release preset.
