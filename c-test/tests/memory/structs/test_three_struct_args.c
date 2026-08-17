void putchar(int c);

struct Pair {
    int a;
    int b;
};

int sum3(struct Pair x, struct Pair y, struct Pair z) {
    return x.a + y.a + z.a + x.b + y.b + z.b;
}

int main() {
    struct Pair p;
    struct Pair q;
    struct Pair r;
    p.a = 1;
    p.b = 2;
    q.a = 3;
    q.b = 4;
    r.a = 5;
    r.b = 6;
    if (sum3(p, q, r) == 21) {
        putchar('Y');
    } else {
        putchar('N');
    }
    putchar('\n');
    return 0;
}
