/* The first visible outdoor sky exposed a 16-bit int truncation of this fixed
 * coordinate. Keep the actual upstream declaration and initializer in this test. */
#include <stdint.h>
#include <stdio.h>
#include "m_fixed.h"
#include "r_sky.h"

int main(void)
{
    fixed_t fraction;
    if (sizeof(skytexturemid) != sizeof(fixed_t)) return 1;
    R_InitSkyMap();
    if (skytexturemid != 100L * FRACUNIT) return 2;
    /* E1M1 forward movement exposes a sky pixel at y43, with centery84.
     * The midpoint must sample row59; a truncated midpoint samples row87. */
    fraction = skytexturemid + (fixed_t)(43 - 84) * FRACUNIT;
    if (((fraction >> FRACBITS) & 127) != 59) return 3;
    puts("SKY OK: fixed-point midpoint and exposed sky row");
    return 0;
}
