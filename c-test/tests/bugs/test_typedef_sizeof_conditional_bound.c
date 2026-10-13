#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct PatchHeader {
    short width, height, left, top;
    int32_t columns[8];
};

/* sizeof and offsetof are integer constant expressions, including as the
 * condition of a file-scope typedef array bound. */
typedef char ShortCell[(sizeof(short) > 0) ? 1 : -1];
typedef char WordCell[(sizeof(uint16_t) == sizeof(short)) ? 1 : -1];
typedef char OffsetCells[(sizeof(int32_t) == 2 * sizeof(uint16_t)) ? 1 : -1];
typedef char HeaderCells[(offsetof(struct PatchHeader, columns) == 4 * sizeof(short)) ? 1 : -1];

int main(void)
{
    putchar(sizeof(ShortCell) == 1 ? 'Y' : 'N');
    putchar(sizeof(WordCell) == 1 ? 'Y' : 'N');
    putchar(sizeof(OffsetCells) == 1 ? 'Y' : 'N');
    putchar(sizeof(HeaderCells) == 1 ? 'Y' : 'N');
    putchar('\n');
    return 0;
}
