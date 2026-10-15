#include <stdio.h>

/* External linkage prevents per-file optimization from assuming this is dead.
 * A closed executable link can prove it unreachable and must remove its RAM. */
unsigned char abandoned_storage[63980];
int retained_table[4] = {7, 13, 19, 23};
int *retained_middle = &retained_table[1];
int *retained_end = &retained_table[4];
static int callback(int index) { return retained_table[index]; }
int (*retained_callback)(int) = callback;

int main(void) {
    if (*retained_middle == 13) putchar('Y'); else putchar('N');
    if (retained_end - retained_table == 4) putchar('Y'); else putchar('N');
    if (retained_callback(2) == 19) putchar('Y'); else putchar('N');
    retained_table[1] = 42;
    if (*retained_middle == 42) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
