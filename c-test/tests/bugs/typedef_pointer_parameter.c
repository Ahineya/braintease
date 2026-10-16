#include <stdio.h>
typedef struct item { int value; struct item *next; } item, *itemptr;
typedef itemptr handle;
static int value(register handle p) { return p->value + p->next->value; }
int main(void) { item a,b; a.value=7; b.value=11; a.next=&b; puts(value(&a)==18?"OK":"FAIL"); return 0; }
