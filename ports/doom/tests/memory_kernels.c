/* Exercise ordinary C prototypes, pointer returns and all three asm calls. */
#include <string.h>
static unsigned char global[40];
int main(void)
{
    unsigned char local[40];
    unsigned int i;
    if (memset(global, 123, sizeof(global)) != global) return 1;
    for (i = 0; i < 40; ++i) if (global[i] != 123) return 2;
    if (memcpy(local, global, sizeof(global)) != local) return 3;
    for (i = 0; i < 40; ++i) if (local[i] != 123) return 4;
    for (i = 0; i < 40; ++i) global[i] = i;
    if (memmove(global + 1, global, 39) != global + 1) return 5;
    for (i = 1; i < 40; ++i) if (global[i] != i - 1) return 6;
    if (memmove(global, global + 1, 39) != global) return 7;
    for (i = 0; i < 39; ++i) if (global[i] != i) return 8;
    return 0;
}
