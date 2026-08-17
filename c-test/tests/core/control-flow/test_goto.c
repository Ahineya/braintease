void putchar(int c);

int main() {
    goto skip;
    putchar('N');
skip:
    putchar('Y');

    int n;
    n = 0;
loop:
    n = n + 1;
    if (n < 3) goto loop;
    if (n == 3) putchar('Y'); else putchar('N');

    int i;
    i = 0;
    while (1) {
        i = i + 1;
        if (i == 2) goto out;
    }
    putchar('N');
out:
    if (i == 2) putchar('Y'); else putchar('N');

    goto inner;
    putchar('N');
outer:
    putchar('Y');
    goto done;
inner:
    goto outer;
    putchar('N');
done:

    putchar('\n');
    return 0;
}
