void putchar(int c);

typedef struct {
    int integer;
    int frac;
} pair_t;

typedef struct {
    pair_t x;
    pair_t y;
} outer_t;

pair_t add_pair(pair_t a, pair_t b) {
    pair_t r;
    r.integer = a.integer + b.integer;
    r.frac = a.frac + b.frac;
    return r;
}

int main() {
    outer_t p;
    p.x.integer = 1;
    p.x.frac = 2;
    p.y.integer = 3;
    p.y.frac = 4;

    p.x = add_pair(p.x, p.y);
    if (p.x.integer == 4 && p.x.frac == 6) {
        putchar('Y');
    } else {
        putchar('N');
    }
    putchar('\n');
    return 0;
}
