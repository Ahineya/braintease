void putchar(int c);

void walk(int x) {
    switch (x) {
        case 1:
            putchar('1');
        case 2:
            putchar('2');
            break;
        case 3:
            putchar('3');
            break;
        case 4:
        case 5:
            putchar('X');
            break;
        default:
            putchar('D');
            break;
    }
}

int main() {
    walk(1);
    walk(2);
    walk(3);
    walk(4);
    walk(5);
    walk(9);
    putchar('\n');
    return 0;
}
