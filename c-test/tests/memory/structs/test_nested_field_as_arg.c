void putchar(int c);

struct Inner {
    int a;
    int b;
};

struct Outer {
    struct Inner x;
    struct Inner y;
};

struct Inner ident(struct Inner v) {
    return v;
}

int main() {
    struct Outer p;
    p.x.a = 1;
    p.x.b = 2;
    p.y.a = 3;
    p.y.b = 4;

    struct Inner r = ident(p.x);
    if (r.a == 1 && r.b == 2) {
        putchar('Y');
    } else {
        putchar('N');
    }
    putchar('\n');
    return 0;
}
