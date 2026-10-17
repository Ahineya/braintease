#include <stdio.h>
static char buffer[40];
static char message[sizeof(buffer) + 30];
int main(void) {
    char local[7];
    char other[sizeof(local) + 2];
    if (sizeof(message) == 70 && sizeof(other) == 9) putchar('Y'); else putchar('N');
    putchar('\n'); return 0;
}
