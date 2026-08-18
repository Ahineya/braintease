// Block-scope function declaration must call the existing function,
// not decay to a function-pointer object. c-testsuite 00078.
void putchar(int c);

int ident(char *p) {
    return *p;
}

int bump(char *p) {
    return *p + 1;
}

int main() {
    char s = 'A';
    int ident(char *p);
    int bump(char *);

    if (ident(&s) == 'A') putchar('Y'); else putchar('N');
    if (bump(&s) == 'B') putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
