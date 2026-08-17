#include <stdbool.h>

void putchar(int c);

int main() {
    _Bool b;
    bool t;
    bool f;

    b = 2;
    if (b == 1) putchar('Y'); else putchar('N');

    b = 0;
    if (b == 0) putchar('Y'); else putchar('N');

    t = true;
    f = false;
    if (t) putchar('Y'); else putchar('N');
    if (!f) putchar('Y'); else putchar('N');

    b = (_Bool)5;
    if (b == 1) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
