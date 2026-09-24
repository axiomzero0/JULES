// C oracle for t_super_tput.jules — same computation, gcc-compiled.
#include <stdio.h>
#include <stdint.h>

static int64_t __attribute__((noinline)) work(int64_t a, int64_t b, int64_t c, int64_t d) {
    int64_t s = 0;
    s = s + a * 5;
    s = s + b + b;
    s = s * 3;
    s = s + c * 9;
    s = s - d - d;
    s = s + (a + b) * 2;
    s = s + (c - d) * 5;
    s = s + a * 17;
    s = s + b * 33;
    s = s + c * 65;
    s = s - d * 7;
    s = s + a + b + c + d;
    s = s + a * 3 + b * 5;
    s = s + c * 6 + d * 10;
    s = s + a * 12 + b * 20;
    s = s + c * 24 + d * 40;
    s = s - a - b - c - d;
    s = s + (a + c) * 3;
    s = s + (b + d) * 9;
    s = s + a * 2 + b * 4 + c * 8 + d * 16;
    return s;
}

int main(void) {
    int64_t t = 0;
    for (int64_t i = 0; i < 64; ++i) {
        t = t + work(i, i * 3 + 1, i * 5 + 2, i * 7 + 3);
        t = t + work(i + 11, i + 13, i + 17, i + 19);
    }
    printf("%ld\n", (long)t);
    return 0;
}
