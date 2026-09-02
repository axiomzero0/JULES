/* bench: mandel — scalar nested loops, f64 mul-add chains + compares */
#include <stdio.h>
static long mandel(long w, long h, long max_iter) {
    long total = 0;
    long py = 0;
    while (py < h) {
        double y0 = ((double)py / (double)h) * 2.4 - 1.2;
        long px = 0;
        while (px < w) {
            double x0 = ((double)px / (double)w) * 3.0 - 2.1;
            double x = 0.0;
            double y = 0.0;
            long iter = 0;
            while (iter < max_iter && x * x + y * y <= 4.0) {
                double xt = x * x - y * y + x0;
                y = 2.0 * x * y + y0;
                x = xt;
                iter = iter + 1;
            }
            total = total + iter;
            px = px + 1;
        }
        py = py + 1;
    }
    return total;
}

int main(void) {
    printf("%ld\n", mandel(750L, 750L, 280L));
    return 0;
}
