/* bench: flops — serial f64 multiply-add chains (latency-bound) */
#include <stdio.h>
static double flops(long n) {
    double a = 1.0;
    double b = 0.5;
    double c = 0.9999999;
    double d = 0.000000001;
    double e = 0.1;
    long i = 0;
    while (i < n) {
        a = a * c + d;
        b = b + a * a * e;
        i = i + 1;
    }
    return a + b * 2.0;
}

int main(void) {
    printf("%f\n", flops(80000000L));
    return 0;
}
