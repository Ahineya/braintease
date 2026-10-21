struct pair { unsigned long x; unsigned long y; int *p; };

static struct pair make(unsigned long n, int *p)
{
    struct pair v = {n, n + 1, p};
    return v;
}

static struct pair transform(struct pair v, unsigned n)
{
    if (n) {
        struct pair next = transform(v, n - 1);
        next.x += 3;
        return next;
    }
    v.y += 7;
    return v;
}

static unsigned long sum(struct pair a, struct pair b)
{
    return a.x + b.y;
}

int main(void)
{
    int anchor = 17;
    struct pair (*callback)(unsigned long, int *) = make;
    struct pair a = make(9, &anchor);
    struct pair b = transform(a, 3);
    unsigned i;
    if (a.x != 9 || a.y != 10 || a.p != &anchor)
        return 1;
    if (b.x != 18 || b.y != 17 || *b.p != 17)
        return 2;
    if (sum(callback(30, &anchor), make(40, &anchor)) != 71)
        return 3;
    for (i = 0; i < 9; i++) {
        struct pair c = callback(i, &anchor);
        if (c.x != i || c.y != i + 1UL || c.p != &anchor)
            return 4;
    }
    return 0;
}
