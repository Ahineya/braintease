#include <stdio.h>
struct entry { char *name; long value; int active; };
struct table { struct entry *entries; int count; };
static unsigned padding[70003];
static struct entry entries[4] = {{"zero", 100001L, 1}, {"one", 200002L, 2}, {"two", 300003L, 3}, {"three", 400004L, 4}};
static struct table table = {entries,4};
static struct entry *get_entry(int i) { return &table.entries[i]; }
static struct entry *(*lookup)(int) = get_entry;
int main(void) {
    int i, okay=1;
    padding[70002L]=123;
    for(i=0;i<table.count;i++) {
        struct entry *entry=lookup(i);
        if(entry->value!=(long)(i+1)*100001L || entry->active!=i+1 || !entry->name[0]) okay=0;
        entry->active+=10;
    }
    if(okay && entries[3].active==14 && padding[70002L]==123) putchar('Y'); else putchar('N');
    putchar('\n'); return 0;
}
