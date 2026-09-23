# JULES vs GCC vs LLVM/Clang — runtime benchmark

* Machine: Intel(R) Xeon(R) Processor, 2 cores, Linux (Debian trixie)
* Compilers: julesc (JULES, 89-pass SoN pipeline), gcc 14.2.0, clang 19.1.7 (LLVM)
* Method: warmup + repeated runs pinned to one core; primary metric is per-process CPU time (user+sys), which is immune to the container's cgroup CPU-quota throttling that quantizes wall time; median of N reps; outputs verified identical before timing.
* Workloads are algorithmically identical scalar kernels; all binaries print the same checksum.

## CPU time (median seconds, lower is better)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | 0.0167 | 0.0168 | 0.0163 | 0.0163 | 0.4580 | 0.0934 | 0.0956 | 0.3333 | 0.1545 | 0.1543 |
| tak | 0.1932 | 0.1418 | 0.1929 | 0.1933 | 0.2652 | 0.1572 | 0.0993 | 0.3215 | 0.1830 | 0.1837 |
| primes | 0.2576 | 0.2571 | 0.2571 | 0.2573 | 0.2589 | 0.2567 | 0.2562 | 0.2578 | 0.1588 | 0.1583 |
| mandel | 0.0962 | 0.0960 | 0.0962 | 0.0961 | 0.1977 | 0.0893 | 0.0893 | 0.2197 | 0.0866 | 0.0865 |
| flops | 0.1489 | 0.1481 | 0.1491 | 0.1492 | 0.2701 | 0.1465 | 0.1465 | 0.2703 | 0.1482 | 0.1479 |
| inthash | 0.0915 | 0.0919 | 0.0915 | 0.0917 | 0.1927 | 0.0744 | 0.0744 | 0.2033 | 0.0716 | 0.0715 |
| vecsum | 0.0301 | 0.0326 | 0.0300 | 0.0300 | 0.4283 | 0.0300 | 0.0292 | 0.4773 | 0.0212 | 0.0208 |
| vecmask | 0.1754 | 0.1843 | 0.1739 | 0.1746 | 1.4326 | 0.1579 | 0.1576 | 1.3226 | 0.1576 | 0.1573 |

## Ratio vs gcc -O3 (higher = JULES slower)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | x0.17 | x0.18 | x0.17 | x0.17 | x4.79 | x0.98 | x1.00 | x3.49 | x1.62 | x1.61 |
| tak | x1.95 | x1.43 | x1.94 | x1.95 | x2.67 | x1.58 | x1.00 | x3.24 | x1.84 | x1.85 |
| primes | x1.01 | x1.00 | x1.00 | x1.00 | x1.01 | x1.00 | x1.00 | x1.01 | x0.62 | x0.62 |
| mandel | x1.08 | x1.08 | x1.08 | x1.08 | x2.21 | x1.00 | x1.00 | x2.46 | x0.97 | x0.97 |
| flops | x1.02 | x1.01 | x1.02 | x1.02 | x1.84 | x1.00 | x1.00 | x1.85 | x1.01 | x1.01 |
| inthash | x1.23 | x1.24 | x1.23 | x1.23 | x2.59 | x1.00 | x1.00 | x2.73 | x0.96 | x0.96 |
| vecsum | x1.03 | x1.12 | x1.03 | x1.03 | x14.67 | x1.03 | x1.00 | x16.35 | x0.73 | x0.71 |
| vecmask | x1.11 | x1.17 | x1.10 | x1.11 | x9.09 | x1.00 | x1.00 | x8.39 | x1.00 | x1.00 |

## Compile time (seconds, one shot incl. link)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | 0.020 | 0.019 | 0.019 | 0.021 | 0.031 | 0.058 | 0.059 | 0.059 | 0.063 | 0.067 |
| tak | 0.019 | 0.020 | 0.019 | 0.019 | 0.030 | 0.033 | 0.078 | 0.054 | 0.060 | 0.061 |
| primes | 0.021 | 0.020 | 0.020 | 0.020 | 0.029 | 0.041 | 0.042 | 0.055 | 0.063 | 0.063 |
| mandel | 0.020 | 0.022 | 0.020 | 0.020 | 0.030 | 0.036 | 0.037 | 0.057 | 0.064 | 0.063 |
| flops | 0.019 | 0.020 | 0.020 | 0.020 | 0.035 | 0.036 | 0.036 | 0.060 | 0.066 | 0.065 |
| inthash | 0.021 | 0.021 | 0.021 | 0.020 | 0.032 | 0.033 | 0.036 | 0.054 | 0.079 | 0.059 |
| vecsum | 0.022 | 0.022 | 0.020 | 0.021 | 0.032 | 0.039 | 0.041 | 0.057 | 0.069 | 0.065 |
| vecmask | 0.022 | 0.023 | 0.021 | 0.023 | 0.035 | 0.040 | 0.042 | 0.059 | 0.069 | 0.069 |
