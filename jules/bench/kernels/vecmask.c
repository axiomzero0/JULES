// bench: vecmask — f64 element-wise masked blend (auto-vectorizable via
// if-conversion in gcc/clang; FP reduction stays scalar under strict FP)
#include <stdio.h>
#include <stdlib.h>

static double vecmask(double *x, double *y, long n, long rounds) {
    double total = 0.0;
    for (long r = 0; r < rounds; r++) {
        for (long i = 0; i < n; i++) {
            double v = x[i];
            double w = 0.0;
            if (v > 0.0) { w = v; } else { w = 0.0; }
            y[i] = w;
        }
        double chk = 0.0;
        for (long i = 0; i < n; i++)
            chk += y[i];
        total += chk;
    }
    return total;
}

int main(void) {
    long n = 100000;
    double *x = malloc((size_t)n * sizeof(double));
    double *y = malloc((size_t)n * sizeof(double));
    for (long i = 0; i < n; i++)
        x[i] = (double)((i % 2001) - 1000) * 0.5;
    printf("%f\n", vecmask(x, y, n, 2000));
    free(x);
    free(y);
    return 0;
}
