# JULES vs GCC vs LLVM/Clang — runtime benchmark

* Machine: Intel(R) Xeon(R) Processor, 2 cores, Linux (Debian trixie)
* Compilers: julesc (JULES, 89-pass SoN pipeline), gcc 14.2.0, clang 19.1.7 (LLVM)
* Method: warmup + repeated runs pinned to one core; primary metric is per-process CPU time (user+sys), which is immune to the container's cgroup CPU-quota throttling that quantizes wall time; median of N reps; outputs verified identical before timing.
* Workloads are algorithmically identical scalar kernels; all binaries print the same checksum.

## CPU time (median seconds, lower is better)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | 0.0168 | 0.0168 | 0.0236 | 0.0234 | 0.4566 | 0.0949 | 0.0965 | 0.3424 | 0.1545 | 0.1546 |
| tak | 0.1942 | 0.1515 | 0.1936 | 0.1938 | 0.2656 | 0.1557 | 0.0996 | 0.3199 | 0.1834 | 0.1834 |
| primes | 0.2575 | 0.2574 | 0.2574 | 0.2573 | 0.2583 | 0.2565 | 0.2564 | 0.2583 | 0.1585 | 0.1586 |
| mandel | 0.0963 | 0.0963 | 0.1128 | 0.1129 | 0.1980 | 0.0896 | 0.0897 | 0.2200 | 0.0867 | 0.0867 |
| flops | 0.1494 | 0.1485 | 0.1493 | 0.1492 | 0.2709 | 0.1468 | 0.1468 | 0.2710 | 0.1478 | 0.1479 |
| inthash | 0.0915 | 0.0920 | 0.0940 | 0.0919 | 0.1936 | 0.0744 | 0.0742 | 0.2042 | 0.0714 | 0.0714 |
| vecsum | 0.0328 | 0.0325 | 0.0414 | 0.0412 | 0.4279 | 0.0293 | 0.0294 | 0.4777 | 0.0209 | 0.0208 |
| vecmask | 0.1773 | 0.1783 | 0.5444 | 0.5398 | 1.4409 | 0.1648 | 0.1687 | 1.3322 | 0.1622 | 0.1582 |

## Ratio vs gcc -O3 (higher = JULES slower)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | x0.17 | x0.17 | x0.24 | x0.24 | x4.73 | x0.98 | x1.00 | x3.55 | x1.60 | x1.60 |
| tak | x1.95 | x1.52 | x1.94 | x1.95 | x2.67 | x1.56 | x1.00 | x3.21 | x1.84 | x1.84 |
| primes | x1.00 | x1.00 | x1.00 | x1.00 | x1.01 | x1.00 | x1.00 | x1.01 | x0.62 | x0.62 |
| mandel | x1.07 | x1.07 | x1.26 | x1.26 | x2.21 | x1.00 | x1.00 | x2.45 | x0.97 | x0.97 |
| flops | x1.02 | x1.01 | x1.02 | x1.02 | x1.85 | x1.00 | x1.00 | x1.85 | x1.01 | x1.01 |
| inthash | x1.23 | x1.24 | x1.27 | x1.24 | x2.61 | x1.00 | x1.00 | x2.75 | x0.96 | x0.96 |
| vecsum | x1.12 | x1.11 | x1.41 | x1.40 | x14.55 | x1.00 | x1.00 | x16.25 | x0.71 | x0.71 |
| vecmask | x1.05 | x1.06 | x3.23 | x3.20 | x8.54 | x0.98 | x1.00 | x7.90 | x0.96 | x0.94 |

## Compile time (seconds, one shot incl. link)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | 0.020 | 0.019 | 0.028 | 0.029 | 0.038 | 0.057 | 0.063 | 0.094 | 0.062 | 0.061 |
| tak | 0.020 | 0.020 | 0.019 | 0.019 | 0.033 | 0.034 | 0.080 | 0.056 | 0.061 | 0.060 |
| primes | 0.021 | 0.020 | 0.020 | 0.019 | 0.030 | 0.038 | 0.040 | 0.056 | 0.062 | 0.062 |
| mandel | 0.020 | 0.021 | 0.019 | 0.020 | 0.031 | 0.037 | 0.050 | 0.059 | 0.065 | 0.064 |
| flops | 0.020 | 0.021 | 0.021 | 0.021 | 0.030 | 0.037 | 0.033 | 0.060 | 0.063 | 0.061 |
| inthash | 0.021 | 0.022 | 0.026 | 0.021 | 0.030 | 0.081 | 0.033 | 0.055 | 0.061 | 0.061 |
| vecsum | 0.022 | 0.021 | 0.022 | 0.021 | 0.033 | 0.041 | 0.039 | 0.059 | 0.069 | 0.068 |
| vecmask | 0.022 | 0.022 | 0.021 | 0.023 | 0.043 | 0.042 | 0.046 | 0.140 | 0.094 | 0.071 |
