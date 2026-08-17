void putchar(int c);

void classify(int x) {
    switch (x) {
        case 1:
            putchar('A');
            break;
        case 2:
            putchar('B');
            break;
        default:
            putchar('D');
            break;
    }
}

int main() {
    classify(1);
    classify(2);
    classify(3);
    classify(0);
    putchar('\n');
    return 0;
}
