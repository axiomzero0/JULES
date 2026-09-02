/* bench: primes — trial division, integer div/mod + short-circuit loops */
#include <stdio.h>
static long count_primes(long limit) {
    long count = 0;
    long n = 3;
    while (n < limit) {
        long is_p = 1;
        long d = 3;
        while (d * d <= n) {
            if (n % d == 0) {
                is_p = 0;
                break;
            }
            d = d + 2;
        }
        if (is_p == 1) {
            count = count + 1;
        }
        n = n + 2;
    }
    return count;
}

int main(void) {
    printf("%ld\n", count_primes(2000000L));
    return 0;
}
