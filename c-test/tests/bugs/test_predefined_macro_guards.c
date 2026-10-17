#include <stdio.h>
#ifdef __FILE__
#define HAVE_FILE 1
#else
#define HAVE_FILE 0
#endif
#ifndef __LINE__
#define HAVE_LINE 0
#else
#define HAVE_LINE 1
#endif
int main(void) {
    if (HAVE_FILE) putchar('Y'); else putchar('N');
    if (HAVE_LINE) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
