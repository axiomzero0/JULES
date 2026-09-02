/* bench: tak — deep recursion, 3-way nested calls */
#include <stdio.h>
static long tak(long x, long y, long z) {
    if (y < x) {
        return tak(tak(x - 1, y, z), tak(y - 1, z, x), tak(z - 1, x, y));
    }
    return z;
}

int main(void) {
    printf("%ld\n", tak(33L, 22L, 12L));
    return 0;
}
