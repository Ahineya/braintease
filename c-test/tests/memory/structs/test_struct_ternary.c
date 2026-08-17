void putchar(int c);

struct Point {
    int x;
    int y;
};

int main() {
    struct Point a;
    struct Point b;
    a.x = 1;
    a.y = 2;
    b.x = 9;
    b.y = 8;

    struct Point c = 1 ? a : b;
    if (c.x == 1 && c.y == 2) {
        putchar('Y');
    } else {
        putchar('N');
    }

    struct Point d = 0 ? a : b;
    if (d.x == 9 && d.y == 8) {
        putchar('Y');
    } else {
        putchar('N');
    }

    putchar('\n');
    return 0;
}
