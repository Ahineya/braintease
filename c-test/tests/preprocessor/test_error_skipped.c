void putchar(int c);

#if 0
#error this should not fire
#endif

int main() {
    putchar('Y');
    putchar('\n');
    return 0;
}
