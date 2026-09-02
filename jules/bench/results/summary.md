# JULES vs GCC vs LLVM/Clang — runtime benchmark

* Machine: Intel(R) Xeon(R) Processor, 2 cores, Linux (Debian trixie)
* Compilers: julesc (JULES, 89-pass SoN pipeline), gcc 14.2.0, clang 19.1.7 (LLVM)
* Method: warmup + repeated runs pinned to one core; primary metric is per-process CPU time (user+sys), which is immune to the container's cgroup CPU-quota throttling that quantizes wall time; median of N reps; outputs verified identical before timing.
* Workloads are algorithmically identical scalar kernels; all binaries print the same checksum.

## CPU time (median seconds, lower is better)

| kernel | julesc-aot | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|
| fib | 0.4778 | 0.4625 | 0.4607 | 0.4591 | 0.0955 | 0.0975 | 0.3368 | 0.1556 | 0.1556 |
| tak | 0.3488 | 0.3495 | 0.3486 | 0.2706 | 0.1577 | 0.1006 | 0.3220 | 0.1846 | 0.1844 |
| primes | 0.4806 | 0.4866 | 0.4867 | 0.2598 | 0.2583 | 0.2578 | 0.2582 | 0.1593 | 0.1592 |
| mandel | 1.0496 | 1.0480 | 1.0423 | 0.1998 | 0.0902 | 0.0997 | 0.2228 | 0.0874 | 0.0872 |
| flops | 0.6514 | 0.6500 | 0.6526 | 0.2728 | 0.1476 | 0.1470 | 0.2721 | 0.1485 | 0.1485 |
| inthash | 0.4402 | 0.4406 | 0.4403 | 0.1941 | 0.0752 | 0.0748 | 0.2044 | 0.0719 | 0.0722 |

## Ratio vs gcc -O3 (higher = JULES slower)

| kernel | julesc-aot | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|
| fib | x4.90 | x4.74 | x4.73 | x4.71 | x0.98 | x1.00 | x3.45 | x1.60 | x1.60 |
| tak | x3.47 | x3.47 | x3.47 | x2.69 | x1.57 | x1.00 | x3.20 | x1.83 | x1.83 |
| primes | x1.86 | x1.89 | x1.89 | x1.01 | x1.00 | x1.00 | x1.00 | x0.62 | x0.62 |
| mandel | x10.53 | x10.51 | x10.45 | x2.00 | x0.90 | x1.00 | x2.23 | x0.88 | x0.87 |
| flops | x4.43 | x4.42 | x4.44 | x1.86 | x1.00 | x1.00 | x1.85 | x1.01 | x1.01 |
| inthash | x5.89 | x5.89 | x5.89 | x2.59 | x1.01 | x1.00 | x2.73 | x0.96 | x0.97 |

## Compile time (seconds, one shot incl. link)

| kernel | julesc-aot | julesc-jit-baseline | julesc-jit-optimizing | gcc-O0 | gcc-O2 | gcc-O3 | clang-O0 | clang-O2 | clang-O3 |
|---|---|---|---|---|---|---|---|---|---|
| fib | 0.027 | 0.023 | 0.020 | 0.032 | 0.057 | 0.063 | 0.062 | 0.067 | 0.065 |
| tak | 0.022 | 0.019 | 0.021 | 0.033 | 0.036 | 0.087 | 0.057 | 0.063 | 0.062 |
| primes | 0.021 | 0.021 | 0.022 | 0.036 | 0.040 | 0.040 | 0.057 | 0.066 | 0.073 |
| mandel | 0.020 | 0.022 | 0.021 | 0.031 | 0.041 | 0.044 | 0.060 | 0.064 | 0.068 |
| flops | 0.023 | 0.020 | 0.022 | 0.033 | 0.036 | 0.036 | 0.060 | 0.066 | 0.061 |
| inthash | 0.020 | 0.020 | 0.030 | 0.031 | 0.038 | 0.035 | 0.087 | 0.069 | 0.105 |
