void putchar(int c);

int main() {
    int i;
    for (i = 0; i < 4; i = i + 1) {
        switch (i) {
            case 0:
                putchar('A');
                break;
            case 1:
                putchar('B');
                continue;
            case 2:
                putchar('C');
                break;
            default:
                putchar('D');
                break;
        }
        putchar('.');
    }
    putchar('\n');
    return 0;
}
