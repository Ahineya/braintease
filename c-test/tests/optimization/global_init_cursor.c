#include <stdio.h>

static unsigned int prefix[] = {17, 65535U, 0, 32768U};
static char text[] = "A\0B";
static long wide[] = {0x12345678L, -1L, 65536L};
static long long wider[] = {0x123456789abcdefLL, -1LL};
static int add(int value) { return value + 7; }
struct Entry { long value; unsigned int *pointer; int (*call)(int); char *text; };
static struct Entry entries[] = {
    { -1L, prefix + 1, add, text + 2 },
    { 65536L, prefix + 3, add, text },
};

int main(void)
{
    if (prefix[0] == 17 && prefix[1] == 65535U && prefix[2] == 0
        && prefix[3] == 32768U) putchar('Y'); else putchar('N');
    if (text[0] == 'A' && text[1] == 0 && text[2] == 'B'
        && text[3] == 0) putchar('Y'); else putchar('N');
    if (wide[0] == 0x12345678L && wide[1] == -1L && wide[2] == 65536L
        && wider[0] == 0x123456789abcdefLL && wider[1] == -1LL)
        putchar('Y'); else putchar('N');
    if (entries[0].value == -1L && *entries[0].pointer == 65535U
        && entries[0].call(5) == 12 && *entries[0].text == 'B')
        putchar('Y'); else putchar('N');
    if (entries[1].value == 65536L && *entries[1].pointer == 32768U
        && entries[1].call(9) == 16 && *entries[1].text == 'A')
        putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
