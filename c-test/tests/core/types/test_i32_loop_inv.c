void putchar(int c);

static void yn(int c) {
    putchar(c ? 'Y' : 'N');
}

int main() {
    unsigned long d0;
    unsigned long s;
    unsigned long nr0;
    int i;
    int cnt;

    d0 = 2;
    s = 0;
    for (i = 0; i < 64; i++) {
        s = s + d0;
    }
    yn(s == 128UL);

    d0 = 2;
    nr0 = 0;
    cnt = 0;
    for (i = 0; i < 64; i++) {
        if (nr0 >= d0) {
            cnt = cnt + 1;
        }
        nr0 = 0;
    }
    yn(cnt == 0);

    d0 = 2;
    nr0 = 0;
    cnt = 0;
    for (i = 0; i < 64; i++) {
        unsigned long msb;
        unsigned long nr1;
        unsigned long d1;
        msb = 0;
        nr1 = 0;
        d1 = 0;
        if (msb || nr1 > d1 || (nr1 == d1 && nr0 >= d0)) {
            cnt = cnt + 1;
        }
    }
    yn(cnt == 0);

    putchar('\n');
    return 0;
}
