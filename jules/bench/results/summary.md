# JULES vs GCC vs LLVM/Clang — runtime benchmark

* Machine: Intel(R) Xeon(R) Processor, 2 cores, Linux (Debian trixie)
* Compilers: julesc (JULES, 89-pass SoN pipeline), gcc 14.2.0, clang 19.1.7 (LLVM)
* Method: warmup + repeated runs pinned to one core; primary metric is per-process CPU time (user+sys), which is immune to the container's cgroup CPU-quota throttling that quantizes wall time; median of N reps; outputs verified identical before timing.
* Workloads are algorithmically identical scalar kernels; all binaries print the same checksum.

## CPU time (median seconds, lower is better)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | 0.0167 | 0.0167 | 0.0161 | 0.0161 | 0.4566 | 0.0934 | 0.0958 | 0.3318 | 0.1550 | 0.1547 |
| tak | 0.1933 | 0.1425 | 0.1937 | 0.1935 | 0.2657 | 0.1555 | 0.0992 | 0.3208 | 0.1833 | 0.1836 |
| primes | 0.2575 | 0.2572 | 0.2572 | 0.2573 | 0.2585 | 0.2567 | 0.2566 | 0.2582 | 0.1585 | 0.1583 |
| mandel | 0.0962 | 0.0961 | 0.0964 | 0.0960 | 0.1981 | 0.0899 | 0.0899 | 0.2199 | 0.0869 | 0.0868 |
| flops | 0.1494 | 0.1484 | 0.1492 | 0.1491 | 0.2704 | 0.1468 | 0.1493 | 0.2713 | 0.1480 | 0.1481 |
| inthash | 0.0919 | 0.0922 | 0.0918 | 0.0916 | 0.1925 | 0.0741 | 0.0743 | 0.2031 | 0.0717 | 0.0713 |
| vecsum | 0.0300 | 0.0299 | 0.0300 | 0.0303 | 0.4276 | 0.0293 | 0.0292 | 0.4773 | 0.0209 | 0.0209 |
| vecmask | 0.1980 | 0.1874 | 0.1976 | 0.1954 | 1.4318 | 0.1621 | 0.1624 | 1.3272 | 0.1589 | 0.1681 |

## Ratio vs gcc -O3 (higher = JULES slower)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | x0.17 | x0.17 | x0.17 | x0.17 | x4.77 | x0.97 | x1.00 | x3.46 | x1.62 | x1.61 |
| tak | x1.95 | x1.44 | x1.95 | x1.95 | x2.68 | x1.57 | x1.00 | x3.23 | x1.85 | x1.85 |
| primes | x1.00 | x1.00 | x1.00 | x1.00 | x1.01 | x1.00 | x1.00 | x1.01 | x0.62 | x0.62 |
| mandel | x1.07 | x1.07 | x1.07 | x1.07 | x2.20 | x1.00 | x1.00 | x2.45 | x0.97 | x0.97 |
| flops | x1.00 | x0.99 | x1.00 | x1.00 | x1.81 | x0.98 | x1.00 | x1.82 | x0.99 | x0.99 |
| inthash | x1.24 | x1.24 | x1.24 | x1.23 | x2.59 | x1.00 | x1.00 | x2.73 | x0.97 | x0.96 |
| vecsum | x1.03 | x1.02 | x1.03 | x1.04 | x14.64 | x1.00 | x1.00 | x16.35 | x0.72 | x0.72 |
| vecmask | x1.22 | x1.15 | x1.22 | x1.20 | x8.82 | x1.00 | x1.00 | x8.17 | x0.98 | x1.04 |

## Compile time (seconds, one shot incl. link)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | 0.019 | 0.018 | 0.020 | 0.020 | 0.030 | 0.053 | 0.057 | 0.054 | 0.078 | 0.058 |
| tak | 0.019 | 0.019 | 0.019 | 0.020 | 0.031 | 0.033 | 0.079 | 0.053 | 0.059 | 0.058 |
| primes | 0.019 | 0.020 | 0.020 | 0.020 | 0.029 | 0.038 | 0.039 | 0.054 | 0.060 | 0.081 |
| mandel | 0.019 | 0.020 | 0.034 | 0.020 | 0.032 | 0.037 | 0.037 | 0.056 | 0.086 | 0.063 |
| flops | 0.020 | 0.021 | 0.019 | 0.019 | 0.029 | 0.033 | 0.033 | 0.062 | 0.065 | 0.060 |
| inthash | 0.027 | 0.021 | 0.020 | 0.020 | 0.031 | 0.033 | 0.034 | 0.054 | 0.066 | 0.062 |
| vecsum | 0.021 | 0.022 | 0.021 | 0.020 | 0.034 | 0.039 | 0.041 | 0.056 | 0.068 | 0.064 |
| vecmask | 0.021 | 0.024 | 0.023 | 0.022 | 0.041 | 0.041 | 0.042 | 0.057 | 0.070 | 0.072 |
