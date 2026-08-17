// Inner Doom post loop: while (i < length) over unsigned short length.

void putchar(int c);

int main() {
    unsigned short pixels[4];
    unsigned short length;
    unsigned short i;
    unsigned short sum;

    pixels[0] = 10;
    pixels[1] = 20;
    pixels[2] = 30;
    pixels[3] = 99;

    length = 3;
    i = 0;
    sum = 0;
    while (i < length) {
        sum = sum + pixels[i];
        i = i + 1;
    }

    if (sum == 60) {
        putchar('Y');
    } else {
        putchar('N');
    }
    if (i == 3) {
        putchar('Y');
    } else {
        putchar('N');
    }

    putchar('\n');
    return 0;
}
