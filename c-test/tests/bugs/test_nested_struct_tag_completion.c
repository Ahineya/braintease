#include <stdio.h>

typedef struct Item Item;
struct Outer {
    struct Item { int value; } *item;
};

int main(void)
{
    Item item;
    struct Outer outer;
    item.value = 7;
    outer.item = &item;
    putchar(outer.item->value == 7 ? 'Y' : 'N');
    return 0;
}
