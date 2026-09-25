#include <stdio.h>
#include <stdlib.h>

typedef struct { long a, b; } Inner;
typedef struct { double x, y; Inner inner; } Point;
typedef struct Node { long val; struct Node *next; } Node;

static void bump(Inner *inner) { inner->a += 100; inner->b += 200; }

int main(void) {
    Point p = { 2.0, 3.0, { 4, 5 } };
    printf("%f\n", p.x); printf("%f\n", p.y);
    printf("%ld\n", p.inner.a); printf("%ld\n", p.inner.b);
    printf("%f\n", p.x * p.y);
    p.x *= 2.0; p.y *= 2.0;
    printf("%f\n", p.x);
    printf("%f\n", p.x * p.y);
    printf("%ld\n", p.inner.a + p.inner.b);

    Point q = p;
    q.x = 10.0;
    printf("%f\n", q.x);
    printf("%f\n", p.x);

    Point r = { 1.0, 1.0, { 0, 0 } };
    bump(&r.inner);
    printf("%ld\n", r.inner.a);
    printf("%ld\n", r.inner.b);

    Point m = { 1.5, 2.5, { 1, 2 } };
    printf("%f\n", m.x);
    printf("%ld\n", m.inner.a);
    printf("%ld\n", m.inner.a * 10 + m.inner.b);

    Point *arr = malloc(3 * sizeof(Point));
    for (long i = 0; i < 3; ++i) {
        arr[i].x = 1.0 * (double)i;
        arr[i].y = 2.0;
        arr[i].inner.a = i;
        arr[i].inner.b = i * 2;
    }
    printf("%f\n", arr[2].x);
    printf("%ld\n", arr[2].inner.b);

    Node *n1 = malloc(sizeof(Node));
    Node *n2 = malloc(sizeof(Node));
    n1->val = 7; n1->next = n2;
    n2->val = 8; n2->next = n1;
    printf("%ld\n", n1->next->val);
    printf("%ld\n", n2->next->val);
    printf("%ld\n", n1->next->next->val);

    { long p2 = 42; printf("%ld\n", p2); }
    printf("%f\n", p.x);

    printf("%ld\n", n1->next->next->next->val);
    free(n1); free(n2); free(arr);
    return 0;
}
