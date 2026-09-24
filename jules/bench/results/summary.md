# JULES vs GCC vs LLVM/Clang — runtime benchmark

* Machine: Intel(R) Xeon(R) Processor, 2 cores, Linux (Debian trixie)
* Compilers: julesc (JULES, 89-pass SoN pipeline), gcc 14.2.0, clang 19.1.7 (LLVM)
* Method: warmup + repeated runs pinned to one core; primary metric is per-process CPU time (user+sys), which is immune to the container's cgroup CPU-quota throttling that quantizes wall time; median of N reps; outputs verified identical before timing.
* Workloads are algorithmically identical scalar kernels; all binaries print the same checksum.

## CPU time (median seconds, lower is better)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | 0.0168 | 0.0168 | 0.0163 | 0.0162 | 0.4602 | 0.0952 | 0.0971 | 0.3332 | 0.1552 | 0.1549 |
| tak | 0.1949 | 0.1430 | 0.1946 | 0.1950 | 0.2663 | 0.1562 | 0.0999 | 0.3209 | 0.1842 | 0.1832 |
| primes | 0.2572 | 0.2573 | 0.2573 | 0.2575 | 0.2585 | 0.2566 | 0.2568 | 0.2579 | 0.1585 | 0.1583 |
| mandel | 0.1199 | 0.0961 | 0.1035 | 0.1034 | 0.1977 | 0.0894 | 0.0896 | 0.2197 | 0.0867 | 0.0868 |
| flops | 0.1471 | 0.1482 | 0.1467 | 0.1467 | 0.2703 | 0.1468 | 0.1472 | 0.2708 | 0.1479 | 0.1479 |
| inthash | 0.0915 | 0.0917 | 0.0914 | 0.0914 | 0.1926 | 0.0742 | 0.0742 | 0.2040 | 0.0719 | 0.0719 |

## Ratio vs gcc -O3 (higher = JULES slower)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | x0.17 | x0.17 | x0.17 | x0.17 | x4.74 | x0.98 | x1.00 | x3.43 | x1.60 | x1.60 |
| tak | x1.95 | x1.43 | x1.95 | x1.95 | x2.67 | x1.56 | x1.00 | x3.21 | x1.84 | x1.83 |
| primes | x1.00 | x1.00 | x1.00 | x1.00 | x1.01 | x1.00 | x1.00 | x1.00 | x0.62 | x0.62 |
| mandel | x1.34 | x1.07 | x1.16 | x1.15 | x2.21 | x1.00 | x1.00 | x2.45 | x0.97 | x0.97 |
| flops | x1.00 | x1.01 | x1.00 | x1.00 | x1.84 | x1.00 | x1.00 | x1.84 | x1.00 | x1.00 |
| inthash | x1.23 | x1.24 | x1.23 | x1.23 | x2.60 | x1.00 | x1.00 | x2.75 | x0.97 | x0.97 |

## Compile time (seconds, one shot incl. link)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | 0.020 | 0.028 | 0.020 | 0.020 | 0.030 | 0.057 | 0.063 | 0.075 | 0.065 | 0.063 |
| tak | 0.021 | 0.044 | 0.021 | 0.022 | 0.034 | 0.035 | 0.091 | 0.054 | 0.058 | 0.056 |
| primes | 0.020 | 0.026 | 0.020 | 0.020 | 0.030 | 0.037 | 0.039 | 0.053 | 0.060 | 0.059 |
| mandel | 0.020 | 0.030 | 0.020 | 0.020 | 0.031 | 0.036 | 0.037 | 0.055 | 0.066 | 0.069 |
| flops | 0.020 | 0.022 | 0.020 | 0.020 | 0.030 | 0.034 | 0.034 | 0.057 | 0.062 | 0.061 |
| inthash | 0.019 | 0.030 | 0.019 | 0.020 | 0.031 | 0.033 | 0.034 | 0.056 | 0.063 | 0.065 |
