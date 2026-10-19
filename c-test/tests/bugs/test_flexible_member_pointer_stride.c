#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
/* A flexible tail contributes no storage to either sizeof or pointer strides. */
struct Tail { unsigned long count; unsigned long cells[]; };
struct Plain { unsigned long count; };
int main(void) {
 struct Tail *p = malloc(sizeof(struct Tail) + 8 * sizeof(unsigned long));
 unsigned long *tail;
 if (!p) return 1;
 if (sizeof(struct Tail) == sizeof(struct Plain)) putchar('Y'); else putchar('N');
 if ((char *)(p + 1) - (char *)p == sizeof(struct Tail)) putchar('Y'); else putchar('N');
 tail = (unsigned long *)(p + 1);
 if (tail == p->cells) putchar('Y'); else putchar('N');
 p->count = 4; p->cells[0] = 70000UL; tail[1] = 90000UL;
 if (p->cells[1] == 90000UL && tail[0] == 70000UL && p->count == 4) putchar('Y'); else putchar('N');
 if ((char *)(p + 3) - (char *)p == 3 * sizeof(struct Tail)) putchar('Y'); else putchar('N');
 free(p); putchar('\n'); return 0;
}
