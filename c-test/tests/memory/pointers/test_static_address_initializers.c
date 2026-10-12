#include <stdint.h>
#include <stdio.h>

#ifndef BANK_SIZE
#define BANK_SIZE 64000
#endif

struct Item { char tag; long count; int cells[3]; };
static char relocation_padding[6000];
extern struct Item items[3];
static int *member = &items[2].cells[1];
static int *arrow = (&items[2])->cells + 2;
static int *back = &items[1].cells[2] - 2;
static char *cast = (char *)&items[2] - sizeof(struct Item);
struct Item items[3] = {{1, 2L, {3, 4, 5}}, {6, 7L, {8, 9, 10}}, {11, 12L, {13, 14, 15}}};
static int matrix[3][3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
static int *nested = &matrix[2][1];
static int (*row)[3] = matrix + 1;
static int *selected = 1 ? &matrix[1][2] : &matrix[2][0];
static char *text = "address" + 2;
struct Node { int value; struct Node *next; };
static struct Node nodes[2] = {{11, &nodes[1]}, {22, &nodes[0]}};
static int buttons[4];
static int *mousebuttons = &buttons[1];
static int *pointers[3] = {buttons, &buttons[2], &buttons[3] - 1};

/* Ripple's integer-to-pointer ABI exposes bank and offset. These addresses are
   compared, never dereferenced, to exercise initializer normalization itself. */
static char *cross = (char *)((5UL << 16) | (BANK_SIZE - 1)) + 2;
static char *borrow = (char *)(6UL << 16) - 1;
static char *carry = (char *)((5UL << 16) | (BANK_SIZE - 1)) + 60000L;
static char *far = (char *)((5UL << 16) | 7) + 140000L;
static char *far_back = (char *)((8UL << 16) | 7) - 140000L;
static char *relocated_carry = (char *)items + 60000L;
static char *relocated_far = (char *)items + 140000L;

int main(void) {
    unsigned long linear;
    if (member != &items[2].cells[1] || *member != 14) return 1;
    if (arrow != &items[2].cells[2] || *arrow != 15) return 2;
    if (back != &items[1].cells[0] || *back != 8) return 3;
    if (cast != (char *)&items[1]) return 4;
    if (nested != &matrix[2][1] || *nested != 8) return 5;
    if (row != &matrix[1] || (*row)[2] != 6) return 6;
    if (selected != &matrix[1][2] || *selected != 6) return 7;
    if (text[0] != 'd' || text[4] != 's' || text[5] != 0) return 8;
    if (nodes[0].next != &nodes[1] || nodes[1].next != &nodes[0]) return 9;
    if (nodes[0].next->value != 22) return 10;
    mousebuttons[-1] = 19;
    if (buttons[0] != 19 || pointers[0] != buttons || pointers[1] != &buttons[2] || pointers[2] != &buttons[2]) return 11;
    if ((uintptr_t)cross != ((6UL << 16) | 1)) return 12;
    if ((uintptr_t)borrow != ((5UL << 16) | (BANK_SIZE - 1))) return 13;
    if ((uintptr_t)carry != ((6UL << 16) | 59999UL)) return 14;
    linear = 5UL * BANK_SIZE + 7 + 140000UL;
    if ((uintptr_t)far != ((linear / BANK_SIZE << 16) | (linear % BANK_SIZE))) return 15;
    linear = 8UL * BANK_SIZE + 7 - 140000UL;
    if ((uintptr_t)far_back != ((linear / BANK_SIZE << 16) | (linear % BANK_SIZE))) return 16;
    relocation_padding[5999] = 23;
    if (relocation_padding[5999] != 23) return 17;
    linear = ((uintptr_t)items >> 16) * BANK_SIZE + ((uintptr_t)items & 65535UL) + 60000UL;
    if ((uintptr_t)relocated_carry != ((linear / BANK_SIZE << 16) | (linear % BANK_SIZE))) return 18;
    linear = ((uintptr_t)items >> 16) * BANK_SIZE + ((uintptr_t)items & 65535UL) + 140000UL;
    if ((uintptr_t)relocated_far != ((linear / BANK_SIZE << 16) | (linear % BANK_SIZE))) return 19;
    puts("static address initializers ok");
    return 0;
}
