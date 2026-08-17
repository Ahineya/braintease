#include <stdio.h>

void putchar(int c);

float addf(float a, float b) {
    return a + b;
}

int main() {
    if (sizeof(float) == 4) putchar('Y'); else putchar('N');
    if (sizeof(double) == 8) putchar('Y'); else putchar('N');

    if (1.0f + 2.0f == 3.0f) putchar('Y'); else putchar('N');
    if (2.0f * 3.0f == 6.0f) putchar('Y'); else putchar('N');
    if (6.0f / 2.0f == 3.0f) putchar('Y'); else putchar('N');
    if (3.0f - 1.0f == 2.0f) putchar('Y'); else putchar('N');

    if (2.0f < 3.0f) putchar('Y'); else putchar('N');
    if (3.0f > 2.0f) putchar('Y'); else putchar('N');
    if (2.0f <= 2.0f) putchar('Y'); else putchar('N');
    if (1.0f != 2.0f) putchar('Y'); else putchar('N');

    if (-1.0f + 1.0f == 0.0f) putchar('Y'); else putchar('N');

    float z = 0.0f;
    if (z) putchar('N'); else putchar('Y');
    float nz = 1.0f;
    if (nz) putchar('Y'); else putchar('N');

    float x = 1.0;
    if (x == 1.0f) putchar('Y'); else putchar('N');

    if (addf(1.5f, 2.5f) == 4.0f) putchar('Y'); else putchar('N');

    if ((int)3.7f == 3) putchar('Y'); else putchar('N');
    if ((float)2 == 2.0f) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
