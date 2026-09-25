#include <stdio.h>
static long f(long x) { x = x + 1; x = x * 2; return x; }
static double g(double y) { y = y * 3.0; return y; }
static long h(long z, long w) { z = z + w; return z; }
int main(void) {
    printf("%ld\n", f(10));
    printf("%f\n", g(1.5));
    printf("%ld\n", h(3, 4));
    long t = 5;
    printf("%ld\n", f(t));
    printf("%ld\n", t);
    return 0;
}
