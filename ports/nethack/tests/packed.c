#include <stdio.h>
#include "../config/ripple-pack.h"
#define CHECK(c) do { if (!(c)) { printf("FAIL packed line %d\n",__LINE__); return 1; } } while (0)
int main(void)
{
    int x = 79, y = 20;
    long packed;
    unsigned i;
    for (i = 0; i < 256; ++i) CHECK(NH_SIGNED_OCTET(i) == (i < 128 ? (long)i : (long)i - 256));
    packed = SP_COORD_PACK(x,y);
    CHECK(SP_COORD_X(packed) == 79 && SP_COORD_Y(packed) == 20);
    packed = SP_COORD_PACK(-1,-2);
    CHECK(SP_COORD_X(packed) == -1 && SP_COORD_Y(packed) == -2);
    packed = SP_REGION_PACK(-1,20,79,-2);
    CHECK(SP_REGION_X1(packed) == -1 && SP_REGION_Y1(packed) == 20);
    CHECK(SP_REGION_X2(packed) == 79 && SP_REGION_Y2(packed) == -2);
    packed = SP_MONST_PACK(400,-1);
    CHECK(SP_MONST_PM(packed) == 400 && SP_MONST_CLASS(packed) == -1);
    packed = SP_OBJ_PACK(-1,-1);
    CHECK(SP_OBJ_TYP(packed) == -1 && SP_OBJ_CLASS(packed) == -1);
    packed = SP_MAPCHAR_PACK(-1,-2);
    CHECK(SP_MAPCHAR_TYP(packed) == -1 && SP_MAPCHAR_LIT(packed) == -2);
    puts("Packed level operands PASS");
    return 0;
}
