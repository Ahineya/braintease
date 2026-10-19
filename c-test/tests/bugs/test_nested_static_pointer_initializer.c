#include <stdio.h>
struct data {
    unsigned long refs;
    union {
        struct { const char *text; unsigned long length; } plain;
        const void *interned;
    } value;
    int kind;
};
static const struct data empty = { 0, { { "C16", 3 } }, 0 };
static const struct data *get(void) { return &empty; }
int main(void) {
    const struct data *p = get();
    if (p->value.plain.text[0] == 'C') putchar('Y'); else putchar('N');
    if (p->value.plain.text[1] == '1' && p->value.plain.text[2] == '6') putchar('Y'); else putchar('N');
    if (p->value.plain.length == 3 && p->refs == 0 && p->kind == 0) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
