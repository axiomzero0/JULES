// bench: vecsum — i64 array reduction (auto-vectorizable in every compiler)
#include <stdio.h>
#include <stdlib.h>

static long vecsum(long *a, long n, long rounds) {
    long total = 0;
    for (long r = 0; r < rounds; r++) {
        long s = 0;
        for (long i = 0; i < n; i++)
            s += a[i];
        total += s;
    }
    return total;
}

int main(void) {
    long n = 100000;
    long *a = malloc((size_t)n * sizeof(long));
    for (long i = 0; i < n; i++)
        a[i] = (i * i) % 1000 - 500;
    printf("%ld\n", vecsum(a, n, 2000));
    free(a);
    return 0;
}
