/* Reference C16 behavior for the guest native module/linker fixtures. Their
 * actual separate compilation is checked by rcc-portable/native/test_modules.py. */
#include <stdio.h>
#define main module_probe
#define helper main_helper
#include "../../../../rcc-portable/native/tests/modules/main.c"
#undef helper
#define helper library_helper
#define private_counter library_counter
/* The host bootstrap lexer rejects wide string escapes; see the expected-Y
 * RED test test_c16_full_width_string_escape. Keep the same C16 cell values
 * here; the actual portable module fixture still exercises the string. */
#define NATIVE_MODULE_GREETING {'h', 0x1234, 0}
#include "../../../../rcc-portable/native/tests/modules/library.c"
#undef private_counter
#undef helper
#define helper peer_helper
#include "../../../../rcc-portable/native/tests/modules/peer.c"
#undef helper
#undef main
int main(void)
{
    if (module_probe() == 0) putchar('Y'); else putchar('N');
    if (saved(10UL) == 110UL && main_helper(10UL) == 11UL && peer(10UL) == 61UL)
        putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
