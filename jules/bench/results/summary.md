# JULES vs GCC vs LLVM/Clang — runtime benchmark

* Machine: Intel(R) Xeon(R) Processor, 2 cores, Linux (Debian trixie)
* Compilers: julesc (JULES, 89-pass SoN pipeline), gcc 14.2.0, clang 19.1.7 (LLVM)
* Method: warmup + repeated runs pinned to one core; primary metric is per-process CPU time (user+sys), which is immune to the container's cgroup CPU-quota throttling that quantizes wall time; median of N reps; outputs verified identical before timing.
* Workloads are algorithmically identical scalar kernels; all binaries print the same checksum.

## CPU time (median seconds, lower is better)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | 0.3270 | 0.3265 | 0.3265 | 0.3263 | 0.4578 | 0.1334 | 0.0958 | 0.3343 | 0.1550 | 0.1546 |
| tak | 0.3357 | 0.3363 | 0.3370 | 0.3355 | 0.2674 | 0.1585 | 0.0999 | 0.3228 | 0.1840 | 0.1839 |
| primes | 0.2582 | 0.2573 | 0.2573 | 0.2572 | 0.2589 | 0.2570 | 0.2574 | 0.2585 | 0.1587 | 0.1590 |
| mandel | 0.2210 | 0.2220 | 0.2236 | 0.2234 | 0.1989 | 0.0901 | 0.0905 | 0.2211 | 0.0872 | 0.0872 |
| flops | 0.2048 | 0.2072 | 0.2049 | 0.2050 | 0.2713 | 0.1476 | 0.1473 | 0.2719 | 0.1481 | 0.1485 |
| inthash | 0.0910 | 0.0910 | 0.1132 | 0.1134 | 0.1925 | 0.0744 | 0.0744 | 0.2038 | 0.0715 | 0.0716 |

## Ratio vs gcc -O3 (higher = JULES slower)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | x3.41 | x3.41 | x3.41 | x3.41 | x4.78 | x1.39 | x1.00 | x3.49 | x1.62 | x1.61 |
| tak | x3.36 | x3.37 | x3.37 | x3.36 | x2.68 | x1.59 | x1.00 | x3.23 | x1.84 | x1.84 |
| primes | x1.00 | x1.00 | x1.00 | x1.00 | x1.01 | x1.00 | x1.00 | x1.00 | x0.62 | x0.62 |
| mandel | x2.44 | x2.45 | x2.47 | x2.47 | x2.20 | x1.00 | x1.00 | x2.44 | x0.96 | x0.96 |
| flops | x1.39 | x1.41 | x1.39 | x1.39 | x1.84 | x1.00 | x1.00 | x1.85 | x1.01 | x1.01 |
| inthash | x1.22 | x1.22 | x1.52 | x1.52 | x2.59 | x1.00 | x1.00 | x2.74 | x0.96 | x0.96 |

## Compile time (seconds, one shot incl. link)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | 0.021 | 0.023 | 0.020 | 0.019 | 0.032 | 0.055 | 0.063 | 0.111 | 0.067 | 0.062 |
| tak | 0.019 | 0.025 | 0.023 | 0.021 | 0.033 | 0.038 | 0.117 | 0.061 | 0.062 | 0.060 |
| primes | 0.027 | 0.024 | 0.022 | 0.024 | 0.034 | 0.044 | 0.044 | 0.055 | 0.064 | 0.061 |
| mandel | 0.021 | 0.022 | 0.020 | 0.030 | 0.034 | 0.039 | 0.041 | 0.061 | 0.070 | 0.067 |
| flops | 0.022 | 0.024 | 0.022 | 0.020 | 0.034 | 0.039 | 0.040 | 0.062 | 0.065 | 0.064 |
| inthash | 0.020 | 0.021 | 0.020 | 0.019 | 0.031 | 0.034 | 0.047 | 0.055 | 0.069 | 0.061 |
