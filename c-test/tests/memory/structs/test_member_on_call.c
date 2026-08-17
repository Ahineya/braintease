void putchar(int c);

struct Point {
    int x;
    int y;
};

struct Point make_point(int x, int y) {
    struct Point p;
    p.x = x;
    p.y = y;
    return p;
}

int main() {
    if (make_point(3, 4).x == 3) {
        putchar('Y');
    } else {
        putchar('N');
    }

    if (make_point(3, 4).y == 4) {
        putchar('Y');
    } else {
        putchar('N');
    }

    putchar('\n');
    return 0;
}
