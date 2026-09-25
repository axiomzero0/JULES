# JULES vs GCC vs LLVM/Clang — runtime benchmark

* Machine: Intel(R) Xeon(R) Processor, 2 cores, Linux (Debian trixie)
* Compilers: julesc (JULES, 89-pass SoN pipeline), gcc 14.2.0, clang 19.1.7 (LLVM)
* Method: warmup + repeated runs pinned to one core; primary metric is per-process CPU time (user+sys), which is immune to the container's cgroup CPU-quota throttling that quantizes wall time; median of N reps; outputs verified identical before timing.
* Workloads are algorithmically identical scalar kernels; all binaries print the same checksum.

## CPU time (median seconds, lower is better)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | 0.0166 | 0.0166 | 0.0161 | 0.0161 | 0.4562 | 0.0931 | 0.0962 | 0.3320 | 0.1555 | 0.1555 |
| tak | 0.1942 | 0.1431 | 0.1951 | 0.1946 | 0.2655 | 0.1567 | 0.1002 | 0.3208 | 0.1836 | 0.1835 |
| primes | 0.2577 | 0.2576 | 0.2577 | 0.2575 | 0.2583 | 0.2569 | 0.2564 | 0.2579 | 0.1585 | 0.1584 |
| mandel | 0.1033 | 0.1180 | 0.1230 | 0.1034 | 0.1977 | 0.0895 | 0.0898 | 0.2195 | 0.0867 | 0.0868 |
| flops | 0.1472 | 0.1484 | 0.1469 | 0.1468 | 0.2710 | 0.1469 | 0.1468 | 0.2709 | 0.1480 | 0.1481 |
| inthash | 0.0916 | 0.0918 | 0.0916 | 0.0916 | 0.1934 | 0.0749 | 0.0744 | 0.2044 | 0.0714 | 0.0715 |

## Ratio vs gcc -O3 (higher = JULES slower)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | x0.17 | x0.17 | x0.17 | x0.17 | x4.74 | x0.97 | x1.00 | x3.45 | x1.62 | x1.62 |
| tak | x1.94 | x1.43 | x1.95 | x1.94 | x2.65 | x1.56 | x1.00 | x3.20 | x1.83 | x1.83 |
| primes | x1.01 | x1.00 | x1.01 | x1.00 | x1.01 | x1.00 | x1.00 | x1.01 | x0.62 | x0.62 |
| mandel | x1.15 | x1.31 | x1.37 | x1.15 | x2.20 | x1.00 | x1.00 | x2.44 | x0.97 | x0.97 |
| flops | x1.00 | x1.01 | x1.00 | x1.00 | x1.85 | x1.00 | x1.00 | x1.85 | x1.01 | x1.01 |
| inthash | x1.23 | x1.23 | x1.23 | x1.23 | x2.60 | x1.01 | x1.00 | x2.75 | x0.96 | x0.96 |

## Compile time (seconds, one shot incl. link)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | 0.019 | 0.022 | 0.020 | 0.020 | 0.029 | 0.054 | 0.059 | 0.056 | 0.063 | 0.063 |
| tak | 0.024 | 0.028 | 0.024 | 0.023 | 0.033 | 0.036 | 0.106 | 0.058 | 0.060 | 0.062 |
| primes | 0.021 | 0.023 | 0.022 | 0.022 | 0.031 | 0.040 | 0.043 | 0.056 | 0.064 | 0.061 |
| mandel | 0.022 | 0.034 | 0.024 | 0.022 | 0.031 | 0.037 | 0.037 | 0.058 | 0.061 | 0.061 |
| flops | 0.083 | 0.024 | 0.021 | 0.022 | 0.033 | 0.035 | 0.036 | 0.059 | 0.063 | 0.064 |
| inthash | 0.022 | 0.026 | 0.022 | 0.025 | 0.032 | 0.035 | 0.034 | 0.055 | 0.062 | 0.060 |
