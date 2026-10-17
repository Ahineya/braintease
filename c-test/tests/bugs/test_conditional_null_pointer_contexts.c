#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { if (condition) putchar('Y'); else putchar('N'); } while (0)

typedef unsigned short *Handle;
typedef int (*Callback)(int);
static unsigned short value = 42;
static unsigned short values[2] = {17, 29};
static Handle initialized = 1 ? &value : 0;
static volatile int choose;
static int calls;

static Handle null_first(int missing) { return missing ? 0 : &value; }
static Handle null_last(int present) { return present ? &value : 0; }
static int increment(int x) { return x + 1; }
static Callback callback(int missing) { return missing ? 0 : increment; }
static int is_value(Handle p) { return p == &value; }
static Handle counted(void) { ++calls; return &value; }
static int condition(void) { ++calls; return choose; }

int main(void) {
    int i;
    unsigned short local = 71;
    Handle heap = malloc(sizeof(*heap));
    *heap = 83;
    CHECK(initialized == &value && *initialized == 42);
    for (i = 0; i < 2; ++i) {
        Handle p;
        Callback f;
        char *text;
        choose = i;
        CHECK(null_first(choose) == (i ? (Handle)0 : &value));
        CHECK(null_last(choose) == (i ? &value : (Handle)0));
        p = choose ? 0L : &value;
        CHECK(i ? p == 0 : p == &value && *p == 42);
        p = choose ? &local : (3 - 3);
        CHECK(i ? p == &local && *p == 71 : p == 0);
        p = choose ? (unsigned long)0 : heap;
        CHECK(i ? p == 0 : p == heap && *p == 83);
        CHECK(is_value(choose ? &value : 0) == i);
        p = choose ? 0 : values;
        CHECK(i ? p == 0 : p == values && p[1] == 29);
        p = choose ? values : 0;
        CHECK(i ? p == values && p[0] == 17 : p == 0);
        text = choose ? 0 : "ok";
        CHECK(i ? text == 0 : text[0] == 'o' && text[1] == 'k');
        f = callback(choose);
        CHECK(i ? f == 0 : f != 0 && f(41) == 42);
        f = choose ? &increment : 0;
        CHECK(i ? f != 0 && f(16) == 17 : f == 0);
        p = choose ? 0 : (choose ? 0 : &value);
        CHECK(i ? p == 0 : p == &value);
        calls = 0;
        p = condition() ? 0 : counted();
        CHECK(calls == (i ? 1 : 2) && (i ? p == 0 : p == &value));
    }
    CHECK(sizeof(choose ? 0 : &value) == sizeof(Handle));
    CHECK(sizeof(choose ? values : 0) == sizeof(Handle));
    free(heap);
    putchar('\n');
    return 0;
}
