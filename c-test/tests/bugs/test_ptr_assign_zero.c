/* C99 6.3.2.3: a constant integer 0 is a null pointer constant and
 * may be assigned to any pointer type.
 *
 * Initialization already works:  int *p = 0;  (test_null_pointer)
 * Assignment does not:           p = 0;
 *   → Semantic error: Type mismatch: expected char*, found int
 */
#include <stdio.h>

int main() {
    char *p;

    p = 0;
    if (!p) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
