/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "utils/errors.h"
#include "netsurf/plot_style.h"
#include "../frontend/font.inc"
#include "../frontend/text-layout.inc"
static unsigned long checks;
#define C(x) do { checks++;if(!(x)){fprintf(stderr,"font check failed at %u: %s\n",__LINE__,#x);return 1;} }while(0)
int main(int argc,char **argv)
{
    plot_font_style_t style;int a,b,w,actual;size_t at,next;unsigned cells,face,size,i;
    const char *cluster="éЄ中",*canonical="éЄ中",*word="Hello world";
    unsigned char *saved;unsigned long record;
    memset(&style,0,sizeof(style));style.size=12288;style.weight=400;
    C(nf_load(argc>1?argv[1]:"/usr/share/netsurf/web.nfa"));
    C(nf_style(&style).size==16);
    C(text_width(&style,"iiii",4,&a)==NSERROR_OK);
    C(text_width(&style,"WWWW",4,&b)==NSERROR_OK&&a<b);
    style.family=PLOT_FONT_FAMILY_MONOSPACE;
    C(text_width(&style,"iiii",4,&a)==NSERROR_OK);
    C(text_width(&style,"WWWW",4,&b)==NSERROR_OK&&a==b);
    style.family=PLOT_FONT_FAMILY_SANS_SERIF;
    C(text_width(&style,cluster,strlen(cluster),&a)==NSERROR_OK);
    C(text_width(&style,canonical,strlen(canonical),&b)==NSERROR_OK&&a==b);
    C(nf_cluster(cluster,strlen(cluster),0,&next,&cells)==233&&next==3);
    C(text_position(&style,cluster,strlen(cluster),a,&at,&actual)==NSERROR_OK&&at==strlen(cluster)&&actual==a);
    for(i=0;i<120;i++) {
        C(text_position(&style,cluster,strlen(cluster),i,&at,&actual)==NSERROR_OK);
        C(at==0||at==3||at==5||at==8);
        C(text_width(&style,cluster,at,&w)==NSERROR_OK&&w==actual);
    }
    C(text_width(&style,"Hello",5,&w)==NSERROR_OK);
    C(text_split(&style,word,strlen(word),w,&at,&actual)==NSERROR_OK&&at==5&&actual==w);
    C(text_width(&style,word,strlen(word),&a)==NSERROR_OK);
    style.size=24576;
    C(text_width(&style,word,strlen(word),&b)==NSERROR_OK&&b>a*3/2);
    for(face=0;face<12;face++)for(size=8;size<=64;size++) {
        struct nf_style f={face,size};struct nf_glyph g=nf_glyph(f,'W',1);
        C(g.record&&g.advance>0&&g.width<128&&g.height<128);
        {
            unsigned per=nf_page_size(size),first,j,x,y,row;
            for(first=0;first<nf_count;first+=per) {
                x=y=row=0;
                for(j=first;j<first+per&&j<nf_count;j++) {
                    uint32_t cp=nf_u16(nf_data+20+j*2UL);
                    struct nf_glyph glyph=nf_glyph(f,cp,ru_width(cp));
                    unsigned gw=glyph.width+1,gh=glyph.height+1;
                    if(x+gw>256) { y+=row;x=row=0; }
                    if(gw>256||y+gh>128)fprintf(stderr,"atlas face=%u size=%u page=%u cp=%lu x=%u y=%u w=%u h=%u\n",face,size,first/per,(unsigned long)cp,x,y,gw,gh);
                    C(gw<=256&&y+gh<=128);
                    x+=gw;if(gh>row)row=gh;
                }
            }
        }
    }
    style.size=0x7fffffffL;C(nf_style(&style).size==64);
    style.size=1;C(nf_style(&style).size==8);
    style.size=12288;style.weight=700;style.flags=FONTF_ITALIC;
    C(nf_style(&style).face==3);
    saved=nf_data;nf_data=NULL;nf_count=0;
    C(!nf_load("/no/such/font"));
    /* A shortened file and a corrupt RLE extent are refused before painting. */
    if(argc>2) {
        FILE *file=fopen(argv[2],"wb");C(file!=NULL);C(fwrite(saved,1,19,file)==19);fclose(file);
        C(!nf_load(argv[2]));
        record=20UL+nf_u16(saved+12)*2UL;saved[record+8]=65;
        file=fopen(argv[2],"wb");C(file!=NULL);C(fwrite(saved,1,nf_u32(saved+16),file)==nf_u32(saved+16));fclose(file);
        C(!nf_load(argv[2]));remove(argv[2]);
    }
    free(saved);printf("Fonts: %lu checks passed\n",checks);return 0;
}
