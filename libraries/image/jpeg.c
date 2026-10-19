/* SPDX-License-Identifier: MIT */
#include "internal.h"
#include "jpeg-codec.inc"
/* Validate the bounded baseline container before the small MCU decoder sees
 * it. In particular its original permissive marker/table paths are unsuitable
 * for untrusted web images. JPEG coefficient decoding remains picojpeg. */
static int jpeg_header(const RiByte *p,size_t n,unsigned *width,unsigned *height) {
    size_t at=2,end,where;unsigned marker,len,components=0,ids[3],i,j,kind,index,count,have_frame=0,have_scan=0;
    uint32_t available;unsigned have_dc=0,have_ac=0,have_quant=0,quant[3];
    while(at<n) {
        if(p[at++]!=255)return RI_CORRUPT;
        while(at<n&&p[at]==255)at++;
        if(at>=n)return RI_CORRUPT;marker=p[at++];
        if(marker==217)return have_scan&&at==n?RI_OK:RI_CORRUPT;
        if(!marker||marker==216||(marker>=208&&marker<=215)||marker==1)return RI_CORRUPT;
        if(n-at<2)return RI_CORRUPT;len=(unsigned)p[at]<<8|p[at+1];
        if(len<2||len>n-at)return RI_CORRUPT;where=at+2;end=at+len;
        if(marker==192) {
            if(have_frame||len<8||p[where]!=8)return RI_UNSUPPORTED;
            *height=(unsigned)p[where+1]<<8|p[where+2];*width=(unsigned)p[where+3]<<8|p[where+4];
            components=p[where+5];if(components!=1&&components!=3)return RI_UNSUPPORTED;
            if(len!=8+components*3)return RI_CORRUPT;
            for(i=0;i<components;i++) {
                ids[i]=p[where+6+i*3];quant[i]=p[where+8+i*3];
                if(quant[i]>1)return RI_UNSUPPORTED;
                for(j=0;j<i;j++)if(ids[i]==ids[j])return RI_CORRUPT;
            }
            if(components==3&&ids[0]=='R'&&ids[1]=='G'&&ids[2]=='B')return RI_UNSUPPORTED;
            have_frame=1;
        } else if(marker==219) {
            while(where<end) {
                index=p[where++];if(index&240||index>1)return RI_UNSUPPORTED;
                if(end-where<64)return RI_CORRUPT;
                for(i=0;i<64;i++)if(!p[where+i])return RI_CORRUPT;
                have_quant|=1U<<index;where+=64;
            }
        } else if(marker==196) {
            while(where<end) {
                kind=p[where]>>4;index=p[where++]&15;
                if(kind>1||index>1)return RI_UNSUPPORTED;
                if(end-where<16)return RI_CORRUPT;
                count=0;available=1;
                for(i=0;i<16;i++) {
                    available*=2;
                    if(p[where+i]>=available)return RI_CORRUPT;
                    available-=p[where+i];count+=p[where+i];
                    /* Only availability through the next code length matters;
                     * it is 32 bits for C16's final depth. */
                }
                where+=16;
                if(!count||count>(kind?255:12)||count>end-where)return RI_CORRUPT;
                for(i=0;i<count;i++) {
                    unsigned symbol=p[where+i];
                    if(!kind&&symbol>11)return RI_CORRUPT;
                    if(kind&&((symbol&15)>10||(!(symbol&15)&&symbol!=0&&symbol!=240)))return RI_CORRUPT;
                }
                if(kind)have_ac|=1U<<index;else have_dc|=1U<<index;
                where+=count;
            }
        } else if(marker==218) {
            if(!have_frame||have_scan||len!=6+2*components||p[where++]!=components)return RI_CORRUPT;
            for(i=0;i<components;i++) {
                unsigned dc=p[where+1]>>4,ac=p[where+1]&15;
                if(p[where]!=ids[i]||dc>1||ac>1||!(have_dc&(1U<<dc))||!(have_ac&(1U<<ac))||!(have_quant&(1U<<quant[i])))return RI_CORRUPT;
                where+=2;
            }
            if(p[where]||p[where+1]!=63||p[where+2])return RI_UNSUPPORTED;
            have_scan=1;at=end;
            /* Only byte stuffing and restart markers may appear in this one
             * baseline entropy scan. Require an actual EOI, never synthesize it. */
            while(at<n) {
                if(p[at++]!=255)continue;
                while(at<n&&p[at]==255)at++;
                if(at>=n)return RI_CORRUPT;
                marker=p[at++];
                if(!marker||(marker>=208&&marker<=215))continue;
                if(marker==217)return at==n?RI_OK:RI_CORRUPT;
                return RI_UNSUPPORTED;
            }
            return RI_CORRUPT;
        } else if(marker==221) { if(len!=4)return RI_CORRUPT; }
        else if(marker==238&&end-where>=12&&!memcmp(p+where,"Adobe",5)&&p[where+11]!=1)return RI_UNSUPPORTED;
        else if(!(marker>=224&&marker<=239)&&marker!=254)return RI_UNSUPPORTED;
        at=end;
    }
    return RI_CORRUPT;
}
struct jpeg_input { const RiByte *data;size_t size,at; };
static unsigned char jpeg_read(unsigned char *buf,unsigned char count,unsigned char *got,void *opaque) {
    struct jpeg_input *in=opaque;size_t n=in->size-in->at;
    if(n>count)n=count;memcpy(buf,in->data+in->at,n);in->at+=n;*got=n;return 0;
}
int ri_jpeg(const RiByte *p,size_t n,const RiLimits *limits,RiImage **out) {
    unsigned w=0,h=0,mx,my,x,y,index,px,py;int status;
    RiJpegState *state;pjpeg_image_info_t info;RiImage *im=NULL;
    struct jpeg_input in={p,n,0};RiByte *dest;
    status=jpeg_header(p,n,&w,&h);if(status)return status;
    status=ri_size(w,h,limits);if(status)return status;
    if(sizeof(*state)>limits->work)return RI_LIMIT;
    state=calloc(1,sizeof(*state));if(!state)return RI_MEMORY;
    status=pjpeg_decode_init(state,&info,jpeg_read,&in,0);
    if(status||info.m_width!=(int)w||info.m_height!=(int)h){free(state);return RI_CORRUPT;}
    im=ri_image_new(w,h,limits);if(!im){free(state);return RI_MEMORY;}
    for(my=0;my<(unsigned)info.m_MCUSPerCol;my++)for(mx=0;mx<(unsigned)info.m_MCUSPerRow;mx++) {
        if(pjpeg_decode_mcu(state)){ri_image_free(im);free(state);return RI_CORRUPT;}
        for(y=0;y<(unsigned)info.m_MCUHeight;y++)for(x=0;x<(unsigned)info.m_MCUWidth;x++) {
            px=mx*info.m_MCUWidth+x;py=my*info.m_MCUHeight+y;if(px>=w||py>=h)continue;
            index=(y/8)*128+(x/8)*64+(y%8)*8+x%8;
            dest=im->rgba+((size_t)py*w+px)*4;
            dest[0]=info.m_pMCUBufR[index];dest[1]=info.m_pMCUBufG[index];dest[2]=info.m_pMCUBufB[index];dest[3]=255;
        }
    }
    free(state);*out=im;return RI_OK;
}
