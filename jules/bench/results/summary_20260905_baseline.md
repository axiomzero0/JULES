# JULES vs GCC vs LLVM/Clang — runtime benchmark

* Machine: Intel(R) Xeon(R) Processor, 2 cores, Linux (Debian trixie)
* Compilers: julesc (JULES, 89-pass SoN pipeline), gcc 14.2.0, clang 19.1.7 (LLVM)
* Method: warmup + repeated runs pinned to one core; primary metric is per-process CPU time (user+sys), which is immune to the container's cgroup CPU-quota throttling that quantizes wall time; median of N reps; outputs verified identical before timing.
* Workloads are algorithmically identical scalar kernels; all binaries print the same checksum.

## CPU time (median seconds, lower is better)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | 0.2021 | 0.2030 | 0.2024 | 0.2023 | 0.4579 | 0.0941 | 0.0968 | 0.3348 | 0.1554 | 0.1557 |
| tak | 0.1938 | 0.1231 | 0.1937 | 0.1935 | 0.2672 | 0.1560 | 0.0997 | 0.3199 | 0.1852 | 0.1840 |
| primes | 0.2586 | 0.2580 | 0.2578 | 0.2579 | 0.2592 | 0.2565 | 0.2570 | 0.2630 | 0.1588 | 0.1588 |
| mandel | 0.1083 | 0.1084 | 0.1122 | 0.1124 | 0.1988 | 0.0901 | 0.0898 | 0.2205 | 0.0871 | 0.0872 |
| flops | 0.1497 | 0.1486 | 0.1499 | 0.1495 | 0.2719 | 0.1473 | 0.1471 | 0.2717 | 0.1483 | 0.1484 |
| inthash | 0.0910 | 0.0912 | 0.1135 | 0.1140 | 0.1936 | 0.0748 | 0.0747 | 0.2045 | 0.0716 | 0.0718 |

## Ratio vs gcc -O3 (higher = JULES slower)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | x2.09 | x2.10 | x2.09 | x2.09 | x4.73 | x0.97 | x1.00 | x3.46 | x1.61 | x1.61 |
| tak | x1.94 | x1.23 | x1.94 | x1.94 | x2.68 | x1.56 | x1.00 | x3.21 | x1.86 | x1.85 |
| primes | x1.01 | x1.00 | x1.00 | x1.00 | x1.01 | x1.00 | x1.00 | x1.02 | x0.62 | x0.62 |
| mandel | x1.21 | x1.21 | x1.25 | x1.25 | x2.21 | x1.00 | x1.00 | x2.46 | x0.97 | x0.97 |
| flops | x1.02 | x1.01 | x1.02 | x1.02 | x1.85 | x1.00 | x1.00 | x1.85 | x1.01 | x1.01 |
| inthash | x1.22 | x1.22 | x1.52 | x1.53 | x2.59 | x1.00 | x1.00 | x2.74 | x0.96 | x0.96 |

## Compile time (seconds, one shot incl. link)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | 0.019 | 0.021 | 0.021 | 0.021 | 0.031 | 0.058 | 0.061 | 0.057 | 0.065 | 0.062 |
| tak | 0.021 | 0.022 | 0.021 | 0.021 | 0.039 | 0.036 | 0.082 | 0.058 | 0.064 | 0.064 |
| primes | 0.022 | 0.024 | 0.022 | 0.020 | 0.036 | 0.041 | 0.043 | 0.060 | 0.068 | 0.066 |
| mandel | 0.022 | 0.033 | 0.021 | 0.021 | 0.033 | 0.064 | 0.040 | 0.059 | 0.067 | 0.073 |
| flops | 0.024 | 0.021 | 0.020 | 0.021 | 0.032 | 0.038 | 0.036 | 0.059 | 0.065 | 0.064 |
| inthash | 0.020 | 0.022 | 0.021 | 0.021 | 0.038 | 0.046 | 0.049 | 0.095 | 0.064 | 0.065 |
