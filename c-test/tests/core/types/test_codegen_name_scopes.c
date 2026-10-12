#include <stdio.h>

typedef struct { int width; } Item;
static Item first = {17};
static Item *p[2];
static int value = 23;
static int scalar = 31;
static int callback_result;

static int hide_parameter(int p) { return p + 1; }
static int hide_local(void) { int value = 9; return value; }
static int read_globals(void) { return p[0]->width + value; }
static void callback(int value) { callback_result = value; }
static void call_untyped(void *routine) { ((void (*)(int))routine)(42); }

int main(void)
{
    int local = 7, sum = 0;
    p[0] = &first;
    putchar(hide_parameter(4) == 5 && hide_local() == 9 && read_globals() == 40 ? 'Y' : 'N');
    {
        int p = 12;
        int value[2] = {3, 8};
        int scalar[2] = {1, 2};
        sum += p + value[1] + scalar[0];
    }
    putchar(sum == 21 && p[0]->width == 17 && value == 23 && scalar == 31 ? 'Y' : 'N');
    {
        int local = 10;
        { int local = 20; sum += local; }
        sum += local;
    }
    putchar(local == 7 && sum == 51 ? 'Y' : 'N');
    for (int local = 0; local < 3; ++local) sum += local;
    putchar(local == 7 && sum == 54 ? 'Y' : 'N');
    { int p[2] = {5, 6}; sum += p[1]; }
    { int p = 11; sum += p; }
    putchar(p[0]->width == 17 && sum == 71 ? 'Y' : 'N');
    call_untyped(callback);
    putchar(callback_result == 42 ? 'Y' : 'N');
    putchar('\n');
    return 0;
}
