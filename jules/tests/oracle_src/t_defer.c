#include <stdio.h>
int main(void) {
    long log = 0; (void)log;
    long n = 0, b = 0, it = 0, c = 0, s = 0;

    // scope 1: plain block, defer at close
    { long scope = 10 + 1; printf("%ld\n", scope); } // 11

    // scope 2: nested defers, inner first, then +1, then outer *2
    { long v = 11; v += 5; v += 1; v *= 2; printf("%ld\n", v); } // 34

    // loops: per-iteration defers
    for (long i = 0; i < 3; ++i) { n = n + 1 + 10; }
    printf("%ld\n", n); // 33

    // break: iteration defer runs at break too
    while (1) {
        it = it + 1;
        if (it == 2) { b = b + 7; break; }
        b = b + 1;
        b = b + 7; // body-end defer
    }
    printf("%ld\n", b);   // 15
    printf("%ld\n", it);  // 2

    // continue: iteration defer runs, then next iteration
    for (long i = 0; i < 4; ++i) {
        if (i == 1) { c += 100; continue; }
        c = c + 1;
        c += 100; // body-end defer
    }
    printf("%ld\n", c); // 403

    // single-statement defer
    { s = 4; s = s + 3; }
    printf("%ld\n", s); // 7

    printf("%ld\n", 41L); // fnval(): value fixed before the defer mutates
    printf("%ld\n", 41L); // fnval2()
    return 0;
}
