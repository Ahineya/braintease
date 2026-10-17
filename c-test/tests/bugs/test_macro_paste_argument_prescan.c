#include <stdio.h>
#define SELECT
#define ATTR_TYPE(name) #name, name##_value
enum { SELECT_value = 7 };
struct attr { char *name; int value; };
static struct attr attrs[] = {{ATTR_TYPE(SELECT)}};
int main(void) {
    if (attrs[0].name[0] == 'S' && attrs[0].value == 7) putchar('Y'); else putchar('N');
    putchar('\n'); return 0;
}
