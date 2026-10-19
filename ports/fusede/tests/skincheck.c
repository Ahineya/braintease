/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Same checks execute as native C and as RCC-compiled C16 on JIT RVM. */
#include <stdio.h>
#include "../include/fde_skin.h"
static struct fde_patch patches[9];
static int failed;
#define CHECK(c) do { if (!(c)) { printf("skincheck failed line %u\n", (unsigned)__LINE__); failed = 1; } } while (0)
int main(void)
{
    struct fde_skin skin = {13, 17, 2, 3, 4, 5, 1};
    int n;
    n = fde_slice(&skin, 101, 63, patches);
    CHECK(n == 9);
    CHECK(patches[0].sw == 2 && patches[0].sh == 3 && patches[0].dw == 2 && patches[0].dh == 3);
    CHECK(patches[4].sx == 2 && patches[4].sy == 3 && patches[4].sw == 7 && patches[4].sh == 9);
    CHECK(patches[4].dx == 2 && patches[4].dy == 3 && patches[4].dw == 95 && patches[4].dh == 55);
    CHECK(patches[8].dx == 97 && patches[8].dy == 58 && patches[8].dw == 4 && patches[8].dh == 5);
    skin.centre = 0; CHECK(fde_slice(&skin, 101, 63, patches) == 8);
    skin.centre = 1; CHECK(fde_slice(&skin, 6, 8, patches) == 4);
    CHECK(fde_slice(&skin, 5, 8, patches) == -1);
    CHECK(fde_slice(&skin, 6, 7, patches) == -1);
    patches[0].sx = 123;
    skin.left = 65535U; skin.right = 1;
    CHECK(fde_slice(&skin, 100, 100, patches) == -1 && patches[0].sx == 123);
    skin.left = 13; skin.right = 0; CHECK(fde_slice(&skin, 100, 100, patches) == -1);
    skin.left = 0; skin.top = 0; skin.bottom = 0;
    CHECK(fde_slice(&skin, 32767, 32767, patches) == 1);
    CHECK(patches[0].dw == 32767 && patches[0].dh == 32767);
    CHECK(fde_slice(&skin, 32768U, 100, patches) == -1);
    CHECK(fde_slice(&skin, 100, 0, patches) == -1);
    CHECK(fde_slice(0, 100, 100, patches) == -1);
    CHECK(fde_slice(&skin, 100, 100, 0) == -1);
    skin.centre = 2; CHECK(fde_slice(&skin, 100, 100, patches) == -1);
    if (!failed) puts("FDE_SKIN_OK");
    return failed;
}
