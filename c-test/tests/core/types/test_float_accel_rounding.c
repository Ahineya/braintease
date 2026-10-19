#include <stdio.h>
#define C(x) do { if (x) putchar('Y'); else putchar('N'); } while (0)
int main(void) {
    union { double d; unsigned long long u; } a, b, r;
    union { float f; unsigned long u; } x, y, z;
    volatile double da, db;
    volatile float fa, fb;
    a.u = 0x3ff0000000000000ULL; b.u = 0x3ca0000000000000ULL;
    da = a.d; db = b.d; r.d = da + db; C(r.u == a.u);
    a.u++; da = a.d; r.d = da + db; C(r.u == a.u + 1ULL);
    a.u = 3; da = a.d; db = 0.5; r.d = da * db; C(r.u == 2);
    da = 1.; db = 3.; r.d = da / db; C(r.u == 0x3fd5555555555555ULL);
    da = -0.; db = 2.; r.d = da * db; C(r.u == 0x8000000000000000ULL);
    x.u = 0x3f800000UL; y.u = 0x33800000UL;
    fa = x.f; fb = y.f; z.f = fa + fb; C(z.u == x.u);
    x.u++; fa = x.f; z.f = fa + fb; C(z.u == x.u + 1UL);
    x.u = 3; fa = x.f; fb = 0.5f; z.f = fa * fb; C(z.u == 2);
    fa = 1.f; fb = 3.f; z.f = fa / fb; C(z.u == 0x3eaaaaabUL);
    fa = -0.f; fb = 2.f; z.f = fa * fb; C(z.u == 0x80000000UL);
    putchar('\n'); return 0;
}
