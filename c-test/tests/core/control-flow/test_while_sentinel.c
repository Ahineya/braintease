// Doom patch columns: posts until a 0xFF row marker.
// while(1) + if (row == 0xFF) return; must actually terminate.

void putchar(int c);

int main() {
    unsigned short data[8];
    unsigned short i;
    unsigned short row;
    unsigned short n;
    unsigned short guard;

    data[0] = 0;
    data[1] = 2;
    data[2] = 10;
    data[3] = 11;
    data[4] = 0xFF;
    data[5] = 99;
    data[6] = 99;
    data[7] = 99;

    i = 0;
    n = 0;
    guard = 0;
    while (1) {
        row = data[i];
        i = i + 1;
        if (row == 0xFF) {
            break;
        }
        n = n + 1;
        guard = guard + 1;
        if (guard > 20) {
            putchar('N');
            putchar('\n');
            return 1;
        }
    }

    if (n == 4) {
        putchar('Y');
    } else {
        putchar('N');
    }

    if (i == 5) {
        putchar('Y');
    } else {
        putchar('N');
    }

    putchar('\n');
    return 0;
}
