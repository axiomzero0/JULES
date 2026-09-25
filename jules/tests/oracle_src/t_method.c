#include <stdio.h>
#include <stdlib.h>
typedef struct { double x, y; } Vec2;
typedef struct { long n; } Counter;

static double dot(const Vec2 *a, const Vec2 *b) {
    return a->x * b->x + a->y * b->y;
}
static double norm2(const Vec2 *v) { return v->x * v->x + v->y * v->y; }
static Vec2 scaled(const Vec2 *v, double k) { Vec2 r = { v->x * k, v->y * k }; return r; }
static void grow(Vec2 *v, double by) { v->x += by; v->y += by; }

int main(void) {
    Vec2 a = { 3.0, 4.0 }, b = { 1.0, 2.0 };
    printf("%f\n", dot(&a, &b));
    printf("%f\n", norm2(&a));
    grow(&a, 1.0);
    printf("%f\n", a.x);
    printf("%f\n", norm2(&a));
    Vec2 c = scaled(&a, 10.0);
    printf("%f\n", c.x);
    printf("%f\n", c.y);
    Vec2 t2 = scaled(&a, 2.0);
    printf("%f\n", norm2(&t2));
    Vec2 *pa = malloc(sizeof(Vec2));
    pa->x = 5.0; pa->y = 12.0;
    printf("%f\n", norm2(pa));
    grow(pa, 0.0);
    printf("%f\n", pa->x);
    Counter *k = malloc(sizeof(Counter));
    k->n = 0; k->n++; k->n++; k->n++;
    printf("%ld\n", k->n);
    Vec2 *arr = malloc(2 * sizeof(Vec2));
    arr[0].x = 1.0; arr[0].y = 1.0; arr[1].x = 6.0; arr[1].y = 8.0;
    printf("%f\n", norm2(&arr[1]));
    grow(&arr[0], 3.0);
    printf("%f\n", arr[0].x);
    free(pa); free(k); free(arr);
    return 0;
}
