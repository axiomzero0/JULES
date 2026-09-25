#include <stdio.h>
#include <math.h>
#include <stdlib.h>
int main(void) {
    printf("%f\n", sqrt(16.0));
    printf("%f\n", sqrt(2.0) * sqrt(2.0));
    printf("%f\n", fabs(-3.5));
    printf("%f\n", fmax(1.25, 2.75));
    printf("%f\n", fmax(10.0, -0.5));
    printf("%ld\n", labs(-1234567890123L));
    printf("%d\n", abs(-42));
    double x = 9.0;
    printf("%f\n", sqrt(x));
    printf("%f\n", sqrt(x + 7.0));
    return 0;
}
