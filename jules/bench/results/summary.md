# JULES vs GCC vs LLVM/Clang — runtime benchmark

* Machine: Intel(R) Xeon(R) Processor, 2 cores, Linux (Debian trixie)
* Compilers: julesc (JULES, 89-pass SoN pipeline), gcc 14.2.0, clang 19.1.7 (LLVM)
* Method: warmup + repeated runs pinned to one core; primary metric is per-process CPU time (user+sys), which is immune to the container's cgroup CPU-quota throttling that quantizes wall time; median of N reps; outputs verified identical before timing.
* Workloads are algorithmically identical scalar kernels; all binaries print the same checksum.

## CPU time (median seconds, lower is better)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | 0.2372 | 0.2375 | 0.2451 | 0.2453 | 0.4580 | 0.0938 | 0.1103 | 0.3336 | 0.1552 | 0.1551 |
| tak | 0.3297 | 0.3279 | 0.3296 | 0.3315 | 0.2665 | 0.1561 | 0.0995 | 0.3198 | 0.1830 | 0.1841 |
| primes | 0.2572 | 0.2574 | 0.2572 | 0.2567 | 0.2586 | 0.2566 | 0.2574 | 0.2589 | 0.1589 | 0.1588 |
| mandel | 0.1071 | 0.1070 | 0.1123 | 0.1123 | 0.1977 | 0.0900 | 0.0900 | 0.2201 | 0.0872 | 0.0870 |
| flops | 0.1498 | 0.1495 | 0.1498 | 0.1498 | 0.2724 | 0.1473 | 0.1473 | 0.2714 | 0.1483 | 0.1484 |
| inthash | 0.0911 | 0.0910 | 0.1136 | 0.1137 | 0.1944 | 0.0748 | 0.0746 | 0.2038 | 0.0720 | 0.0718 |

## Ratio vs gcc -O3 (higher = JULES slower)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | x2.15 | x2.15 | x2.22 | x2.22 | x4.15 | x0.85 | x1.00 | x3.02 | x1.41 | x1.41 |
| tak | x3.31 | x3.30 | x3.31 | x3.33 | x2.68 | x1.57 | x1.00 | x3.21 | x1.84 | x1.85 |
| primes | x1.00 | x1.00 | x1.00 | x1.00 | x1.00 | x1.00 | x1.00 | x1.01 | x0.62 | x0.62 |
| mandel | x1.19 | x1.19 | x1.25 | x1.25 | x2.20 | x1.00 | x1.00 | x2.45 | x0.97 | x0.97 |
| flops | x1.02 | x1.01 | x1.02 | x1.02 | x1.85 | x1.00 | x1.00 | x1.84 | x1.01 | x1.01 |
| inthash | x1.22 | x1.22 | x1.52 | x1.52 | x2.61 | x1.00 | x1.00 | x2.73 | x0.97 | x0.96 |

## Compile time (seconds, one shot incl. link)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | 0.020 | 0.030 | 0.021 | 0.021 | 0.032 | 0.058 | 0.063 | 0.128 | 0.072 | 0.066 |
| tak | 0.020 | 0.022 | 0.021 | 0.020 | 0.033 | 0.038 | 0.085 | 0.060 | 0.064 | 0.065 |
| primes | 0.022 | 0.021 | 0.021 | 0.022 | 0.033 | 0.042 | 0.043 | 0.057 | 0.067 | 0.065 |
| mandel | 0.020 | 0.023 | 0.021 | 0.033 | 0.037 | 0.046 | 0.052 | 0.068 | 0.074 | 0.071 |
| flops | 0.024 | 0.022 | 0.022 | 0.023 | 0.032 | 0.038 | 0.037 | 0.064 | 0.068 | 0.065 |
| inthash | 0.023 | 0.022 | 0.022 | 0.022 | 0.032 | 0.036 | 0.041 | 0.062 | 0.069 | 0.069 |
