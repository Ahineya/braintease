// Anonymous struct/union members — c-testsuite 00046 / 00050.
void putchar(int c);

typedef struct {
    int a;
    union {
        int b1;
        int b2;
    };
    struct { union { struct { int c; }; }; };
    struct {
        int d;
    };
} s;

struct S2 {
    int a;
    int b;
    union {
        int c;
        int d;
    };
    struct {
        int x;
        int y;
    } nested;
};

struct S2 v = {1, 2, 3, {4, 5}};

int main() {
    s u;
    u.a = 1;
    u.b1 = 2;
    u.c = 3;
    u.d = 4;

    if (u.a == 1) putchar('Y'); else putchar('N');
    if (u.b1 == 2 && u.b2 == 2) putchar('Y'); else putchar('N');
    if (u.c == 3) putchar('Y'); else putchar('N');
    if (u.d == 4) putchar('Y'); else putchar('N');

    if (v.a == 1) putchar('Y'); else putchar('N');
    if (v.b == 2) putchar('Y'); else putchar('N');
    if (v.c == 3 || v.d == 3) putchar('Y'); else putchar('N');
    if (v.nested.x == 4 && v.nested.y == 5) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
