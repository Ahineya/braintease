#include <stdio.h>

int shared = 4;
static int private_shared = 30;
int entries[3] = {1, 2, 3};

static int update(void)
{
    extern int shared;
    extern int private_shared;
    extern int entries[];
    shared += 7;
    private_shared++;
    entries[2] = shared;
    return shared;
}
static int shadow(void)
{
    int shared = 99;
    {
        extern int shared;
        shared++;
    }
    return shared;
}
static int definition_later(void)
{
    extern int later;
    extern int later_array[];
    later++;
    return later + later_array[2];
}
static int prototype(void)
{
    extern int external_function(int);
    typedef long Wide;
    Wide local = 70000L;
    return external_function(5) + (local == 70000L);
}

int later = 9;
int later_array[3] = {2, 4, 6};
int external_function(int x) { return x * 3; }

int main(void)
{
    putchar(update() == 11 && shared == 11 && private_shared == 31 && entries[2] == 11 ? 'Y' : 'N');
    putchar(shadow() == 99 && shared == 12 ? 'Y' : 'N');
    putchar(definition_later() == 16 && later == 10 ? 'Y' : 'N');
    putchar(prototype() == 16 ? 'Y' : 'N');
    putchar('\n');
    return 0;
}
