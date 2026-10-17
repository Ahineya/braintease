#include <stdio.h>
static unsigned pad[63996];
static struct {
    unsigned marker;
    unsigned long first;
    unsigned spacer;
    unsigned long second;
} wide = {17, 0x1234abcdUL, 18, 0xdeadbeefUL};
static void check(int value) { putchar(value ? 'Y' : 'N'); }
int main(void) {
    check(pad[0] == 0 && pad[63995] == 0);
    check(wide.marker == 17 && wide.spacer == 18);
    check(wide.first == 0x1234abcdUL && wide.second == 0xdeadbeefUL);
    wide.first = 0xbeef7654UL;
    wide.second = 0x87654321UL;
    check(wide.first == 0xbeef7654UL && wide.second == 0x87654321UL);
    check(wide.marker == 17 && wide.spacer == 18 && pad[63995] == 0);
    putchar('\n');
    return 0;
}
