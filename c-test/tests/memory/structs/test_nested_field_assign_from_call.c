void putchar(int c);

struct Inner {
    int a;
    int b;
};

struct Outer {
    struct Inner x;
    struct Inner y;
};

struct Inner make(int a, int b) {
    struct Inner v;
    v.a = a;
    v.b = b;
    return v;
}

int main() {
    struct Outer p;
    p.x = make(7, 8);
    if (p.x.a == 7 && p.x.b == 8) {
        putchar('Y');
    } else {
        putchar('N');
    }
    putchar('\n');
    return 0;
}
