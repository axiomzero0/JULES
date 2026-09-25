#include <stdio.h>
typedef double Scalar;
typedef struct { Scalar x, y; } Point;
typedef Point *PairPtr;
typedef long Count;
static Scalar norm(const Point *p) { return p->x * p->x + p->y * p->y; }
int main(void) {
    Point p = { 3.0, 4.0 };
    printf("%f\n", p.x);
    Point q = p;
    q.x = 10.0;
    printf("%f\n", q.x);
    printf("%f\n", p.x);
    Scalar r = 2.5;
    printf("%f\n", r);
    PairPtr pp = &p;
    printf("%f\n", pp->x);
    printf("%f\n", norm(pp));
    Count n = 7;
    printf("%ld\n", n);
    const long K = 3;
    printf("%ld\n", K);
    return 0;
}
