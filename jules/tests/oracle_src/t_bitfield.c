#include <stdio.h>
typedef unsigned long Flags;
#define KIND(w) (((w) >> 0) & 7u)
#define FLAG(w) (((w) >> 3) & 1u)
#define SPARE(w) (((w) >> 4) & 15u)
#define TAG(w) (((w) >> 8) & 4095u)
typedef unsigned long Wide;
#define LOW(w) ((w) & 0xFFFFFFFFu)
#define HIGH(w) (((w) >> 32) & 0xFFFFFFFFu)

int main(void) {
    Flags f = (5u << 0) | (1u << 3) | (0u << 4) | (300u << 8);
    printf("%lu\n%lu\n%lu\n%lu\n", KIND(f), FLAG(f), SPARE(f), TAG(f));
    f = (f & ~(7u << 0)) | (7u << 0);
    printf("%lu\n%lu\n", KIND(f), TAG(f));
    f = (f & ~(1u << 3)) | (0u << 3);
    printf("%lu\n", FLAG(f));
    f = (f & ~(4095u << 8)) | (4095u << 8);
    printf("%lu\n", TAG(f));
    f = (f & ~(4095u << 8)) | (0u << 8);
    printf("%lu\n", TAG(f));
    Flags g = (2u << 0) | (1u << 3) | (15u << 4) | (1u << 8);
    printf("%lu\n", g);
    Flags h = g;
    printf("%lu\n%lu\n%lu\n", KIND(h), SPARE(h), TAG(h));
    Wide w = (12345u) | (7ull << 32);
    printf("%lu\n%lu\n", LOW(w), HIGH(w));
    w = (w & ~(0xFFFFFFFFull << 32)) | (4000000000ull << 32);
    printf("%lu\n%lu\n", HIGH(w), LOW(w));
    return 0;
}
