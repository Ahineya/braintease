#include <stdio.h>

typedef unsigned long cell_size;
typedef void (*tick_fn)(void *);
typedef union { tick_fn tick; } Action;
typedef struct Node {
    struct Node *next;
    Action action;
    cell_size size;
} Node;
typedef struct Zone { cell_size size; Node head; Node *rover; } Zone;

enum { FIRST = 1, LIMIT = 3 } menu;
enum { OTHER = 8 };
static int plus(int x) { return x + 1; }
static int twice(int x) { return x * 2; }
static int (*functions[])(int) = {plus, twice};
static void step(Node *node) { node->size += 70000UL; }
static Zone zone;

int main(void)
{
    Node next;
    Node nodes[2];
    int (*local[])(int) = {twice, plus};
    int (**indirect)(int) = local;
    zone.head.size = 7;
    zone.head.next = &next;
    next.size = 9;
    zone.rover = &zone.head;
    zone.head.action.tick = (tick_fn) step;
    nodes->size = 18;
    zone.rover->action.tick(zone.rover);
    putchar(zone.head.size == 70007UL && zone.rover->next->size == 9 && nodes->size == 18 ? 'Y' : 'N');
    putchar((*functions[1])(5) == 10 && functions[0](5) == 6 ? 'Y' : 'N');
    putchar((*indirect[0])(7) == 14 && local[1](7) == 8 ? 'Y' : 'N');
    menu = FIRST;
    putchar(menu == 1 && LIMIT == 3 && OTHER == 8 ? 'Y' : 'N');
    { enum { TOP = 8, BOTTOM = TOP * 2 }; putchar(TOP == 8 && BOTTOM == 16 ? 'Y' : 'N'); }
    { int LIMIT = 9; putchar(LIMIT == 9 ? 'Y' : 'N'); }
    putchar('\n');
    return 0;
}
