#include <stdio.h>
struct mixed {
    unsigned head;
    unsigned long wide;
    unsigned long long huge;
    const char *text;
};
union storage { struct mixed mixed; unsigned cells[12]; };
struct wrapper { unsigned lead; union storage value; unsigned tail; };
static const struct wrapper items[] = {
    {3, {.mixed = {0x1234, 0x89abcdefUL, 0xfedcba9876543210ULL, "AB"}}, 9},
    {5, {.cells = {0xbeef, 0x1357}}, 7}
};
static const union { double number; unsigned cells[4]; } decimal = {.number = 1.5};
static const union { union storage inner; unsigned cells[16]; } nested = {
    .inner = {.mixed = {42, 70000UL, 0x123456789abcdef0ULL, "XY"}}
};
int main(void) {
    unsigned i, good = 1;
    if (items[0].lead == 3 && items[0].tail == 9 && items[1].lead == 5 && items[1].tail == 7)
        putchar('Y'); else putchar('N');
    if (items[0].value.mixed.wide == 0x89abcdefUL && items[0].value.mixed.huge == 0xfedcba9876543210ULL)
        putchar('Y'); else putchar('N');
    if (items[0].value.mixed.text[0] == 'A' && items[0].value.mixed.text[1] == 'B')
        putchar('Y'); else putchar('N');
    if (items[0].value.cells[1] == 0xcdef && items[0].value.cells[2] == 0x89ab &&
        items[0].value.cells[3] == 0x3210 && items[0].value.cells[6] == 0xfedc)
        putchar('Y'); else putchar('N');
    for (i = 9; i < 12; ++i) if (items[0].value.cells[i]) good = 0;
    for (i = 2; i < 12; ++i) if (items[1].value.cells[i]) good = 0;
    if (good && items[1].value.cells[0] == 0xbeef && items[1].value.cells[1] == 0x1357)
        putchar('Y'); else putchar('N');
    if (!decimal.cells[0] && !decimal.cells[1] && !decimal.cells[2] && decimal.cells[3] == 0x3ff8)
        putchar('Y'); else putchar('N');
    if (nested.inner.mixed.head == 42 && nested.inner.mixed.wide == 70000UL &&
        nested.inner.mixed.huge == 0x123456789abcdef0ULL && nested.inner.mixed.text[1] == 'Y')
        putchar('Y'); else putchar('N');
    putchar('\n'); return 0;
}
