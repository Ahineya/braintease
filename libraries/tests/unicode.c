#include "../../runtime/include/ripple_unicode.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void) {
    const unsigned char text[]={0x41,0xd0,0x96,0xf0,0x9f,0x99,0x82,0};
    const unsigned char accent[]={0x65,0xcc,0x81,'x'};
    const unsigned char emoji[]={0xf0,0x9f,0x91,0xa9,0xe2,0x80,0x8d,0xf0,0x9f,0x92,0xbb,'X'};
    const unsigned char bad[]={0xed,0xa0,0x80,'X'};
    size_t at=0;unsigned w,i;uint32_t g;unsigned char encoded[4];
    assert(ru_next(text,7,&at)=='A'&&at==1);
    assert(ru_next(text,7,&at)==0x416&&at==3);
    assert(ru_next(text,7,&at)==0x1f642UL&&at==7);
    assert(ru_encode(0x1f642UL,encoded)==4&&!memcmp(encoded,text+3,4));
    at=0;for(i=0;i<3;i++)assert(ru_next(bad,4,&at)==RU_REPLACEMENT);assert(ru_next(bad,4,&at)=='X');
    assert(ru_next_grapheme(accent,4,0,&w)==3&&w==1);
    assert(ru_prefix(accent,4,1)==3&&ru_columns(accent,4)==2);
    assert(ru_previous_grapheme(accent,4,3)==0&&ru_previous_grapheme(accent,4,4)==3);
    assert(ru_next_grapheme(emoji,sizeof(emoji),0,&w)==11&&w==2);
    assert(ru_prefix(emoji,sizeof(emoji),1)==0&&ru_columns(emoji,sizeof(emoji))==3);
    assert(ru_width(0x4e2d)==2&&ru_width(0x301)==0&&ru_width(27)==-1);
    g=ru_append('e',0x301);assert(g>=RU_CLUSTER_BASE&&ru_scalar(g,0)=='e'&&ru_scalar(g,1)==0x301&&ru_glyph_width(g)==1);
    assert(g==ru_append('e',0x301));
    assert(ru_font_row(0x416,6)!=0);
    {const unsigned char flags[]={0xf0,0x9f,0x87,0xba,0xf0,0x9f,0x87,0xa6};
     assert(ru_next_grapheme(flags,sizeof(flags),0,&w)==8&&w==2);}
    g=ru_append(0x1f1faUL,0x1f1e6UL);assert(ru_glyph_width(g)==2);
    assert(ru_glyph_width(ru_append(g,0x301))==2);
    puts("unicode: strict UTF-8, graphemes, widths, clipping, interning and font passed");return 0;
}
