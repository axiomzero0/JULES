/* bench: inthash — u64 ALU chain (mul/xor/shift), defined wrap-around */
#include <stdio.h>
int main(void) {
    unsigned long h = 0;
    unsigned long i = 0;
    while (i < 80000000UL) {
        h = h * 31UL + (i ^ (i << 3UL) ^ (i >> 5UL));
        i = i + 1UL;
    }
    printf("%lu\n", h);
    return 0;
}
