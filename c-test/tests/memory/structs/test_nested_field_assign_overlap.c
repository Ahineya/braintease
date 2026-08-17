void putchar(int c);

struct Inner {
    int a;
    int b;
};

struct Outer {
    struct Inner x;
    struct Inner y;
};

struct Inner add_inner(struct Inner a, struct Inner b) {
    struct Inner r;
    r.a = a.a + b.a;
    r.b = a.b + b.b;
    return r;
}

int main() {
    struct Outer p;
    p.x.a = 1;
    p.x.b = 2;
    p.y.a = 3;
    p.y.b = 4;

    p.x = add_inner(p.x, p.y);
    if (p.x.a == 4 && p.x.b == 6) {
        putchar('Y');
    } else {
        putchar('N');
    }
    putchar('\n');
    return 0;
}
