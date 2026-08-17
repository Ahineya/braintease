void putchar(int c);

#define CONCAT(a, b) a##b

int main() {
    int CONCAT(var, 1) = 42;
    if (var1 == 42) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
