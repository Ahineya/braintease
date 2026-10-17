#include <stdio.h>
#include <string.h>
#define SELECT
#define ATTR(x) #x, x##_value
enum { SELECT_value = 7 };
struct attr { const char *name; int value; };
static struct attr attrs[] = {{ATTR(SELECT)}};
#define RAW(x) #x
#define STR(x) RAW(x)
#define BAD(a,b) a+b
#define IGNORE(x) 9
#define N 42
#define BOTH(x) #x, x, x##_value
enum { N_value = 3 };
static struct { const char *name; int expanded, pasted; } both = {BOTH(N)};
#define INC(x) ((x)+1)
#define ALIAS INC
#define CAT(a,b) a##b
#define JOIN(a,b,c) a##b##c
#define VA(...) __VA_ARGS__
#define RAWS(...) #__VA_ARGS__
int main(void) {
    int recursive = 4;
    int abc = 11, ac = 12;
    if (!strcmp(attrs[0].name, "SELECT") && attrs[0].value == 7) putchar('Y'); else putchar('N');
    if (!strcmp(both.name, "N") && both.expanded == 42 && both.pasted == 3) putchar('Y'); else putchar('N');
    if (IGNORE(BAD(1)) == 9 && !strcmp(RAW(BAD(1)), "BAD(1)")) putchar('Y'); else putchar('N');
    if (ALIAS(INC(2)) == 4 && !strcmp(STR(N), "42")) putchar('Y'); else putchar('N');
    if (CAT(ab,c) == 11 && JOIN(a,,c) == 12 && CAT(,abc) == 11 && CAT(abc,) == 11) putchar('Y'); else putchar('N');
    if (!strcmp(RAWS(N, N), "N, N") && (VA(N) == 42)) putchar('Y'); else putchar('N');
    if (!strcmp(RAW("a  b"), "\"a  b\"")) putchar('Y'); else putchar('N');
#define recursive (recursive + 1)
    if (recursive == 5 && recursive == 5) putchar('Y'); else putchar('N');
#undef recursive
    putchar('\n');
    return 0;
}
