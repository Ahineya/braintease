/*	$OpenBSD: michael.c,v 1.2 2008/07/21 19:52:45 damien Exp $	*/

/*
 * Copyright (c) 2005, 2006 Reyk Floeter <reyk@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* Bounded native TKIP Michael MIC, following OpenBSD michael.c1.2.
 * Explicit unsigned octet loads avoid alignment and signed-promotion UB.
 * Input is Ethernet DA/SA, QoS priority and the complete LLC payload. */
#include "wifi.h"
static uint32_t le(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint32_t rol(uint32_t v,unsigned n){return v<<n|v>>(32-n);}
static void block(uint32_t *l,uint32_t *r,uint32_t v) {
    *l^=v;*r^=rol(*l,17);*l+=*r;
    *r^=((*l&0xff00ff00U)>>8)|((*l&0x00ff00ffU)<<8);*l+=*r;
    *r^=rol(*l,3);*l+=*r;*r^=rol(*l,30);*l+=*r;
}
void wifi_michael(const uint8_t key[8],const uint8_t da[6],const uint8_t sa[6],unsigned priority,const uint8_t *p,unsigned bytes,uint8_t out[8]) {
    uint8_t header[16]={0};for(unsigned i=0;i<6;i++){header[i]=da[i];header[6+i]=sa[i];}header[12]=priority;
    uint32_t l=le(key),r=le(key+4),v=0;unsigned count=0;
    for(unsigned i=0;i<16;i+=4)block(&l,&r,le(header+i));
    for(unsigned i=0;i<bytes;i++) {
        v|=(uint32_t)p[i]<<(8*count++);if(count==4){block(&l,&r,v);v=count=0;}
    }
    v|=0x5aU<<(8*count);block(&l,&r,v);block(&l,&r,0);
    for(unsigned i=0;i<4;i++){out[i]=l>>(8*i);out[4+i]=r>>(8*i);}
    wifi_wipe(header,sizeof(header));
}
