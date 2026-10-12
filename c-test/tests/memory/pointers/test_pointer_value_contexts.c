#include <stdio.h>
#include <stdlib.h>

typedef struct { signed int number; short int small; } Item;
static Item first = {7, 8};
static Item second = {9, 10};
static Item *items[] = {&first, &second};
static char *names[] = {"first", "second"};
static Item *empty = 0;

static int null_argument(Item *ptr, int marker) { return ptr == 0 && marker == 42; }
static Item *null_result(void) { return 0; }

int main(void)
{
    Item *ptr = &first;
    Item **indirect = &ptr;
    char *bank = malloc(70010UL);
    char *buffers[2];
    signed int pick = 1;
    if (!bank) return 1;
    bank[64005UL] = 'Q';
    buffers[0] = bank;
    buffers[1] = bank + 64000UL;
    putchar(items[pick]->number == 9 && items[0]->small == 8 ? 'Y' : 'N');
    putchar(names[pick][2] == 'c' && *(names[0] + 1) == 'i' ? 'Y' : 'N');
    putchar((*indirect)->number == 7 && (pick ? items[1] : items[0])->small == 10 ? 'Y' : 'N');
    putchar(buffers[1][5] == 'Q' && *((pick ? buffers[1] : buffers[0]) + 5) == 'Q' ? 'Y' : 'N');
    ptr = 0;
    putchar(ptr == 0 && empty == 0 && null_result() == 0 ? 'Y' : 'N');
    ptr = &second;
    *indirect = 2 - 2;
    putchar(ptr == 0 && null_argument(0, 42) && null_argument(3 - 3, 42) ? 'Y' : 'N');
    putchar('\n');
    free(bank);
    return 0;
}
