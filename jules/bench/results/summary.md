# JULES vs GCC vs LLVM/Clang — runtime benchmark

* Machine: Intel(R) Xeon(R) Processor, 2 cores, Linux (Debian trixie)
* Compilers: julesc (JULES, 89-pass SoN pipeline), gcc 14.2.0, clang 19.1.7 (LLVM)
* Method: warmup + repeated runs pinned to one core; primary metric is per-process CPU time (user+sys), which is immune to the container's cgroup CPU-quota throttling that quantizes wall time; median of N reps; outputs verified identical before timing.
* Workloads are algorithmically identical scalar kernels; all binaries print the same checksum.

## CPU time (median seconds, lower is better)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | 0.0167 | 0.0167 | 0.0161 | 0.0163 | 0.4568 | 0.0945 | 0.0961 | 0.3335 | 0.1556 | 0.1563 |
| tak | 0.1948 | 0.1427 | 0.1942 | 0.1941 | 0.2661 | 0.1557 | 0.0997 | 0.3206 | 0.1840 | 0.1840 |
| primes | 0.2575 | 0.2585 | 0.2580 | 0.2576 | 0.2583 | 0.2566 | 0.2578 | 0.2583 | 0.1589 | 0.1586 |
| mandel | 0.1033 | 0.0965 | 0.1036 | 0.1037 | 0.1978 | 0.0894 | 0.0894 | 0.2194 | 0.0866 | 0.0866 |
| flops | 0.1467 | 0.1483 | 0.1468 | 0.1469 | 0.2713 | 0.1468 | 0.1466 | 0.2711 | 0.1479 | 0.1479 |
| inthash | 0.0915 | 0.0918 | 0.0914 | 0.0915 | 0.1927 | 0.0743 | 0.0742 | 0.2031 | 0.0716 | 0.0714 |
| vecsum | 0.0298 | 0.0295 | 0.0299 | 0.0297 | 0.4296 | 0.0294 | 0.0292 | 0.4771 | 0.0209 | 0.0209 |
| vecmask | 0.1921 | 0.1908 | 0.1905 | 0.1927 | 1.4366 | 0.1641 | 0.1634 | 1.3251 | 0.1641 | 0.1614 |

## Ratio vs gcc -O3 (higher = JULES slower)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | x0.17 | x0.17 | x0.17 | x0.17 | x4.75 | x0.98 | x1.00 | x3.47 | x1.62 | x1.63 |
| tak | x1.95 | x1.43 | x1.95 | x1.95 | x2.67 | x1.56 | x1.00 | x3.22 | x1.85 | x1.85 |
| primes | x1.00 | x1.00 | x1.00 | x1.00 | x1.00 | x1.00 | x1.00 | x1.00 | x0.62 | x0.62 |
| mandel | x1.16 | x1.08 | x1.16 | x1.16 | x2.21 | x1.00 | x1.00 | x2.45 | x0.97 | x0.97 |
| flops | x1.00 | x1.01 | x1.00 | x1.00 | x1.85 | x1.00 | x1.00 | x1.85 | x1.01 | x1.01 |
| inthash | x1.23 | x1.24 | x1.23 | x1.23 | x2.60 | x1.00 | x1.00 | x2.74 | x0.96 | x0.96 |
| vecsum | x1.02 | x1.01 | x1.02 | x1.02 | x14.71 | x1.01 | x1.00 | x16.34 | x0.72 | x0.72 |
| vecmask | x1.18 | x1.17 | x1.17 | x1.18 | x8.79 | x1.00 | x1.00 | x8.11 | x1.00 | x0.99 |

## Compile time (seconds, one shot incl. link)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | 0.019 | 0.022 | 0.019 | 0.019 | 0.030 | 0.057 | 0.061 | 0.057 | 0.060 | 0.063 |
| tak | 0.020 | 0.060 | 0.021 | 0.019 | 0.031 | 0.035 | 0.081 | 0.056 | 0.062 | 0.062 |
| primes | 0.020 | 0.030 | 0.022 | 0.021 | 0.030 | 0.038 | 0.055 | 0.057 | 0.061 | 0.063 |
| mandel | 0.021 | 0.050 | 0.021 | 0.021 | 0.031 | 0.037 | 0.038 | 0.058 | 0.062 | 0.063 |
| flops | 0.021 | 0.021 | 0.020 | 0.020 | 0.029 | 0.035 | 0.034 | 0.058 | 0.061 | 0.063 |
| inthash | 0.019 | 0.023 | 0.019 | 0.020 | 0.031 | 0.034 | 0.034 | 0.056 | 0.061 | 0.062 |
| vecsum | 0.021 | 0.041 | 0.021 | 0.021 | 0.033 | 0.040 | 0.039 | 0.057 | 0.069 | 0.065 |
| vecmask | 0.021 | 0.030 | 0.022 | 0.022 | 0.033 | 0.043 | 0.041 | 0.058 | 0.071 | 0.069 |
