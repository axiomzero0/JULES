#include <stdio.h>
typedef unsigned long Perm;
enum { Read = 1, Write = 2, Exec = 4 };
typedef unsigned long Io;
enum { In = 1, Out = 2, Err = 4, Hup = 64 };
#define has(m, f) (((m) & (f)) != 0)

static long open_mode(Perm m) {
    long n = 0;
    if (has(m, Read)) n += 1;
    if (has(m, Write)) n += 10;
    if (has(m, Exec)) n += 100;
    return n;
}

int main(void) {
    Perm m = Read | Write;
    printf("%d\n%d\n%d\n", has(m, Read), has(m, Write), has(m, Exec));
    printf("%ld\n", open_mode(m));
    printf("%ld\n", open_mode(Read | Exec));
    printf("%ld\n", open_mode(Exec));
    Perm full = Read | Write | Exec;
    Perm not_r = full & ~Read;
    printf("%d\n%d\n", has(not_r, Read), has(not_r, Write));
    Perm x = full ^ Write;
    printf("%d\n", has(x, Write));
    x = x ^ Write;
    printf("%d\n", has(x, Write));
    printf("%d\n%d\n", m == (Read | Write), m == Read);
    unsigned long raw = m; raw |= 64;
    Perm back = raw;
    printf("%d\n%lu\n", has(back, Read), raw);
    Io io = In | Hup;
    printf("%d\n%d\n%d\n", has(io, In), has(io, Out), has(io, Hup));
    return 0;
}
