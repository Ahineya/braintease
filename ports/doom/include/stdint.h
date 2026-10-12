#ifndef RIPPLE_DOOM_STDINT_H
#define RIPPLE_DOOM_STDINT_H
#include "../../../runtime/include/stdint.h"
/* Legacy engine spelling only: these are NOT exact-width eight-bit types.
 * File data is stored one octet (0..255) per 16-bit C byte. Signed serialized
 * values must be sign-extended explicitly; sizeof never measures disk data. */
typedef unsigned char uint8_t;
typedef signed char int8_t;
#endif
