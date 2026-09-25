#include <stdio.h>
typedef enum { Red, Green = 5, Blue } Color;
typedef enum { A = 1, B = 10000000000L, C } Big;
typedef enum { X = 4000000000u, Y } U;
typedef struct { Color c; long heat; } Pixel;

static long classify(Color c) {
    if (c == Red) return 1;
    if (c == Green) return 2;
    if (c == Blue) return 3;
    return 0;
}

int main(void) {
    Color c = Red;
    printf("%ld\n", (long)c);
    c = Green; printf("%ld\n", (long)c);
    c = Blue; printf("%ld\n", (long)c);
    printf("%d\n", c == Blue);
    printf("%d\n", c == Green);
    printf("%ld\n", classify(Red));
    printf("%ld\n", classify(Green));
    printf("%ld\n", classify(Blue));
    Big b = B; printf("%ld\n", (long)b);
    b = C; printf("%ld\n", (long)b);
    U u = X; printf("%lu\n", (unsigned long)u);
    Pixel px = { Green, 3 };
    printf("%ld\n", px.heat);
    printf("%ld\n", (long)px.c);
    const long G = 1 + 2;
    printf("%ld\n", G);
    Color d = (Color)5;
    printf("%d\n", d == Green);
    return 0;
}
