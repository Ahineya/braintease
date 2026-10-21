#include <stdio.h>
#include <string.h>

static int global = 7;
static int *global_pointer = &global;
static int add(int n) { return n + *global_pointer; }
static int twice(int n) { return n * 2; }
static int (*table[2])(int) = {add, twice};
static int invoke(int (*callback)(int), int value) { return callback(value); }
static void check(int condition) { putchar(condition ? 'Y' : 'N'); }
int main(void) {
    check(table[0](35) == 42 && table[1](21) == 42);
    check(invoke(table[0], 35) == 42 && invoke(twice, 21) == 42);
    global = 19;
    check(*global_pointer == 19 && table[0](23) == 42);
    unsigned char cells[12], copy[12];
    memset(cells, 0xffff, sizeof(cells));
    memcpy(copy, cells, sizeof(cells));
    check(copy[0] == 65535U && copy[11] == 65535U && sizeof(void *) == 2);
    putchar('\n');
    return 0;
}
