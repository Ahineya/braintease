#include <stdio.h>

/* Ordinary casts and sizeof must not copy the rest of the translation unit
 * at every checkpoint. Repeated compatible extern declarations exercise
 * parsing without creating a large initializer or runtime allocation.
 */
#define ITEM extern long values[(long)sizeof(long)];
#define R2 ITEM ITEM
#define R4 R2 R2
#define R8 R4 R4
#define R16 R8 R8
#define R32 R16 R16
#define R64 R32 R32
#define R128 R64 R64
#define R256 R128 R128
#define R512 R256 R256
#define R1024 R512 R512
#define R2048 R1024 R1024
#define R4096 R2048 R2048
R4096

int main(void) {
    if (sizeof(values) == sizeof(long) * sizeof(long))
        putchar('Y');
    else putchar('N');
    putchar('\n');
    return 0;
}
