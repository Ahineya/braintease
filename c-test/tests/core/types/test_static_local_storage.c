#include <stdio.h>

struct Node { int value; struct Node *next; };
static int count = 33;

static int counter(void) { static int count = 5; return ++count; }
static int other_counter(void) { static int count = 100; return ++count; }
static int zero_counter(void) { static unsigned long count; return ++count == 1; }
static int array_counter(void)
{
    static int values[] = {2, 3};
    return ++values[1];
}
static struct Node *list(void)
{
    static struct Node head = {7, &head};
    static struct Node *saved = &head;
    return saved;
}
static int sibling_scopes(int which)
{
    if (which) { static int count = 10; return ++count; }
    else { static int count = 20; return ++count; }
}
static int grouped(void)
{
    static int value = 8, *pointer = &value;
    return ++*pointer;
}
static char *text(void)
{
    static char buffer[] = "abc";
    return buffer;
}
static void clobber(void)
{
    int values[32];
    int i;
    for (i = 0; i < 32; ++i) values[i] = i;
}
static int recursive(int depth)
{
    static int value;
    value++;
    if (depth) return recursive(depth - 1);
    return value;
}

int main(void)
{
    struct Node *node;
    char *buffer;
    int first = counter();
    int second = counter();
    putchar(first == 6 && second == 7 && other_counter() == 101 && count == 33 ? 'Y' : 'N');
    first = zero_counter();
    second = zero_counter();
    putchar(first && !second ? 'Y' : 'N');
    first = array_counter();
    second = array_counter();
    putchar(first == 4 && second == 5 ? 'Y' : 'N');
    node = list();
    node->value = 99;
    putchar(node == node->next && list() == node && list()->value == 99 ? 'Y' : 'N');
    first = sibling_scopes(1);
    second = sibling_scopes(0);
    putchar(first == 11 && second == 21 && sibling_scopes(1) == 12 && sibling_scopes(0) == 22 ? 'Y' : 'N');
    first = grouped();
    second = grouped();
    putchar(first == 9 && second == 10 ? 'Y' : 'N');
    buffer = text();
    buffer[1] = 'Z';
    clobber();
    putchar(text() == buffer && buffer[0] == 'a' && buffer[1] == 'Z' && buffer[3] == 0 ? 'Y' : 'N');
    first = recursive(3);
    second = recursive(1);
    putchar(first == 4 && second == 6 ? 'Y' : 'N');
    putchar('\n');
    return 0;
}
