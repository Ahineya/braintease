/* SPDX-License-Identifier: MIT */
#include "../../runtime/include/ripple_unicode.h"
#include "font.inc"
/* Direct-mapped glyph cache; ASCII renderers retain their existing ROM path.
 * No allocation, file reads or font parsing occurs during painting. */
static uint32_t cached_code[64];
static const uint16_t *cached_rows[64];
static const uint16_t *glyph(uint32_t cp) {
    unsigned low=0,high=sizeof(ru_font_codes)/sizeof(ru_font_codes[0]),mid;
    while(low<high){mid=low+(high-low)/2;if(cp<ru_font_codes[mid])high=mid;else if(cp>ru_font_codes[mid])low=mid+1;else return ru_font_rows[mid];}
    if(cp!=RU_REPLACEMENT)return glyph(RU_REPLACEMENT);return NULL;
}
unsigned ru_font_row(uint32_t cp,unsigned row) {
    unsigned slot=(unsigned)cp&63,bits=0,i,n;const uint16_t *p;uint32_t scalar;
    if(row>=16)return 0;
    n=ru_scalar_count(cp);
    for(i=0;i<n;i++){
        scalar=ru_scalar(cp,i);
        if(scalar==0xfe0f||scalar==0xfe0e||scalar==0x200d)continue;
        /* Joined emoji are represented by one fallback; combining marks can
         * overlay the base bitmap. Script shaping remains a separate renderer. */
        if(i&&ru_gcb(scalar)!=RU_EXTEND&&ru_gcb(scalar)!=RU_SPACING)continue;
        slot=(unsigned)scalar&63;
        if(cached_code[slot]!=scalar||!cached_rows[slot]){cached_code[slot]=scalar;cached_rows[slot]=glyph(scalar);}
        p=cached_rows[slot];if(p)bits|=p[row];
    }
    return bits;
}
