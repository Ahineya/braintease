#include <stdio.h>
/* Common compatibility extension used by macros emitting definitions. */
;
#define DEFINE(name, value) static int name(void) { return value; }
DEFINE(answer, 42);
;;
int main(void) {
    if (answer() == 42) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
};
