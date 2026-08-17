void putchar(int c);

struct Big {
    int a;
    int b;
    int c;
    int d;
};

struct Big make_big(void) {
    struct Big m;
    m.a = 1;
    m.b = 2;
    m.c = 3;
    m.d = 4;
    return m;
}

struct Big add_big(struct Big x, struct Big y) {
    struct Big r;
    r.a = x.a + y.a;
    r.b = x.b + y.b;
    r.c = x.c + y.c;
    r.d = x.d + y.d;
    return r;
}

int main() {
    struct Big m = make_big();
    if (m.a == 1 && m.b == 2 && m.c == 3 && m.d == 4) {
        putchar('Y');
    } else {
        putchar('N');
    }

    struct Big n = add_big(m, m);
    if (n.a == 2 && n.b == 4 && n.c == 6 && n.d == 8) {
        putchar('Y');
    } else {
        putchar('N');
    }

    putchar('\n');
    return 0;
}
