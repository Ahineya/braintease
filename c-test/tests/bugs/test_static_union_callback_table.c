#include <stdio.h>
typedef long (*Callback)(long);
static long answer(long n) { return n + 70000L; }
struct method { int length, kind; Callback callback; };
struct entry { int tag; union { struct method method; const char *text; } value; };
static const struct entry table[] = { { 1, { { 2, 3, answer } } }, { 2, { .text = "C16" } } };
int main(void) {
 if (table[0].tag == 1 && table[0].value.method.length == 2 && table[0].value.method.kind == 3) putchar('Y'); else putchar('N');
 if (table[0].value.method.callback(42) == 70042L) putchar('Y'); else putchar('N');
 if (table[1].tag == 2 && table[1].value.text[1] == '1') putchar('Y'); else putchar('N');
 putchar('\n'); return 0;
}
