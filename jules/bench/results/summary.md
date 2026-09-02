# JULES vs GCC vs LLVM/Clang — runtime benchmark

* Machine: Intel(R) Xeon(R) Processor, 2 cores, Linux (Debian trixie)
* Compilers: julesc (JULES, 89-pass SoN pipeline), gcc 14.2.0, clang 19.1.7 (LLVM)
* Method: warmup + repeated runs pinned to one core; primary metric is per-process CPU time (user+sys), which is immune to the container's cgroup CPU-quota throttling that quantizes wall time; median of N reps; outputs verified identical before timing.
* Workloads are algorithmically identical scalar kernels; all binaries print the same checksum.

## CPU time (median seconds, lower is better)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | 0.3372 | 0.3369 | 0.3419 | 0.3412 | 0.4600 | 0.0944 | 0.0965 | 0.3348 | 0.1556 | 0.1556 |
| tak | 0.3327 | 0.3347 | 0.3322 | 0.3342 | 0.2660 | 0.1574 | 0.1010 | 0.3218 | 0.1845 | 0.1841 |
| primes | 0.2576 | 0.2576 | 0.2590 | 0.2576 | 0.2589 | 0.2572 | 0.2571 | 0.2588 | 0.1588 | 0.1589 |
| mandel | 0.4969 | 0.4966 | 0.4965 | 0.4957 | 0.1989 | 0.0900 | 0.0903 | 0.2202 | 0.0871 | 0.0870 |
| flops | 0.5394 | 0.5396 | 0.5385 | 0.5393 | 0.2728 | 0.1476 | 0.1473 | 0.2716 | 0.1484 | 0.1484 |
| inthash | 0.1522 | 0.1513 | 0.1550 | 0.1528 | 0.1925 | 0.0744 | 0.0743 | 0.2035 | 0.0715 | 0.0716 |

## Ratio vs gcc -O3 (higher = JULES slower)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | x3.49 | x3.49 | x3.54 | x3.54 | x4.77 | x0.98 | x1.00 | x3.47 | x1.61 | x1.61 |
| tak | x3.29 | x3.31 | x3.29 | x3.31 | x2.63 | x1.56 | x1.00 | x3.19 | x1.83 | x1.82 |
| primes | x1.00 | x1.00 | x1.01 | x1.00 | x1.01 | x1.00 | x1.00 | x1.01 | x0.62 | x0.62 |
| mandel | x5.50 | x5.50 | x5.50 | x5.49 | x2.20 | x1.00 | x1.00 | x2.44 | x0.96 | x0.96 |
| flops | x3.66 | x3.66 | x3.66 | x3.66 | x1.85 | x1.00 | x1.00 | x1.84 | x1.01 | x1.01 |
| inthash | x2.05 | x2.04 | x2.09 | x2.06 | x2.59 | x1.00 | x1.00 | x2.74 | x0.96 | x0.96 |

## Compile time (seconds, one shot incl. link)

| kernel | julesc-aot | julesc-aot-O3 | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|---|
| fib | 0.020 | 0.019 | 0.019 | 0.019 | 0.031 | 0.059 | 0.064 | 0.062 | 0.066 | 0.065 |
| tak | 0.021 | 0.029 | 0.021 | 0.022 | 0.033 | 0.037 | 0.085 | 0.061 | 0.064 | 0.065 |
| primes | 0.022 | 0.022 | 0.022 | 0.022 | 0.033 | 0.042 | 0.042 | 0.057 | 0.065 | 0.072 |
| mandel | 0.022 | 0.022 | 0.022 | 0.029 | 0.033 | 0.038 | 0.063 | 0.058 | 0.065 | 0.064 |
| flops | 0.019 | 0.020 | 0.020 | 0.021 | 0.030 | 0.038 | 0.037 | 0.062 | 0.064 | 0.065 |
| inthash | 0.022 | 0.021 | 0.022 | 0.021 | 0.030 | 0.035 | 0.036 | 0.059 | 0.063 | 0.082 |
