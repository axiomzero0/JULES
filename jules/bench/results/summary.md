# JULES vs GCC vs LLVM/Clang — runtime benchmark

* Machine: Intel(R) Xeon(R) Processor, 2 cores, Linux (Debian trixie)
* Compilers: julesc (JULES, 89-pass SoN pipeline), gcc 14.2.0, clang 19.1.7 (LLVM)
* Method: warmup + repeated runs pinned to one core; primary metric is per-process CPU time (user+sys), which is immune to the container's cgroup CPU-quota throttling that quantizes wall time; median of N reps; outputs verified identical before timing.
* Workloads are algorithmically identical scalar kernels; all binaries print the same checksum.

## CPU time (median seconds, lower is better)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | 0.3358 | 0.3363 | 0.3423 | 0.3425 | 0.4576 | 0.0944 | 0.0963 | 0.3338 | 0.1550 | 0.1553 |
| tak | 0.3272 | 0.3251 | 0.3263 | 0.3259 | 0.2674 | 0.1564 | 0.1000 | 0.3214 | 0.1844 | 0.1841 |
| primes | 0.2576 | 0.2573 | 0.2581 | 0.2584 | 0.2588 | 0.2577 | 0.2576 | 0.2586 | 0.1587 | 0.1592 |
| mandel | 0.2220 | 0.2222 | 0.2208 | 0.2202 | 0.1988 | 0.0901 | 0.0900 | 0.2203 | 0.0871 | 0.0873 |
| flops | 0.3972 | 0.3953 | 0.3965 | 0.3950 | 0.2712 | 0.1470 | 0.1474 | 0.2755 | 0.1483 | 0.1482 |
| inthash | 0.0948 | 0.0949 | 0.0948 | 0.0952 | 0.1934 | 0.0748 | 0.0747 | 0.2044 | 0.0717 | 0.0716 |

## Ratio vs gcc -O3 (higher = JULES slower)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | x3.49 | x3.49 | x3.55 | x3.56 | x4.75 | x0.98 | x1.00 | x3.47 | x1.61 | x1.61 |
| tak | x3.27 | x3.25 | x3.26 | x3.26 | x2.67 | x1.56 | x1.00 | x3.21 | x1.84 | x1.84 |
| primes | x1.00 | x1.00 | x1.00 | x1.00 | x1.00 | x1.00 | x1.00 | x1.00 | x0.62 | x0.62 |
| mandel | x2.47 | x2.47 | x2.45 | x2.45 | x2.21 | x1.00 | x1.00 | x2.45 | x0.97 | x0.97 |
| flops | x2.69 | x2.68 | x2.69 | x2.68 | x1.84 | x1.00 | x1.00 | x1.87 | x1.01 | x1.01 |
| inthash | x1.27 | x1.27 | x1.27 | x1.27 | x2.59 | x1.00 | x1.00 | x2.74 | x0.96 | x0.96 |

## Compile time (seconds, one shot incl. link)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | 0.019 | 0.019 | 0.020 | 0.020 | 0.029 | 0.055 | 0.060 | 0.056 | 0.067 | 0.066 |
| tak | 0.021 | 0.021 | 0.022 | 0.021 | 0.030 | 0.035 | 0.081 | 0.061 | 0.063 | 0.063 |
| primes | 0.022 | 0.022 | 0.021 | 0.022 | 0.034 | 0.052 | 0.042 | 0.058 | 0.066 | 0.068 |
| mandel | 0.022 | 0.022 | 0.020 | 0.023 | 0.033 | 0.044 | 0.044 | 0.062 | 0.067 | 0.069 |
| flops | 0.021 | 0.020 | 0.020 | 0.020 | 0.032 | 0.034 | 0.036 | 0.059 | 0.061 | 0.067 |
| inthash | 0.021 | 0.022 | 0.021 | 0.021 | 0.032 | 0.037 | 0.037 | 0.060 | 0.064 | 0.086 |

**Geomean julesc-aot-O3 vs gcc-O3: x2.14** (over 6 kernels)
