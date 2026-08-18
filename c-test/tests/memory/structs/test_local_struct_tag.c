// Local tagged struct defined together with a variable, then used as
// `struct S *` — c-testsuite 00018. File-scope `struct S { ... };`
// was already covered; this form never registered the tag.
void putchar(int c);

int main() {
    struct S { int x; int y; } s;
    struct S *p;

    p = &s;
    s.x = 1;
    p->y = 2;
    if (p->y + p->x - 3 == 0) putchar('Y'); else putchar('N');
    if (s.y == 2) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
