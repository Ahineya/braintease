/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host-side logic checks for the wire codec; real RCC/PTY tests are separate. */
#include <assert.h>
#include "../ripple/telnet-protocol.h"
static void feed(struct telnet *t,const unsigned char *p,unsigned size,unsigned char *out,unsigned *n) {
    unsigned i;for(i=0;i<size;i++)assert(tn_feed(t,p[i],out,n,256));
}
int main(void) {
    struct telnet t;unsigned char out[256];unsigned n=0,i,before;
    const unsigned char naws[]={255,251,31,255,250,31,0,255,255,0,64,255,240};
    const unsigned char lines[]={'a',13,10,'b',13,0,'c',255,255};
    const unsigned char encoded[]={'A',13,10,'B',13,0,255,255};
    tn_init(&t,1);assert(t.used==9);
    feed(&t,naws,sizeof(naws),out,&n);assert(t.width==255&&t.height==64);
    feed(&t,lines,sizeof(lines),out,&n);
    assert(n==6&&out[0]=='a'&&out[1]==13&&out[2]=='b'&&out[3]==13&&out[4]=='c'&&out[5]==255);
    feed(&t,(const unsigned char *)"\377\372\037",3,out,&n);
    for(i=0;i<100;i++)assert(tn_feed(&t,'x',out,&n,256));
    feed(&t,(const unsigned char *)"\377\360",2,out,&n);
    assert(t.width==255&&t.height==64&&t.state==0);
    tn_init(&t,0);n=0;
    feed(&t,lines,7,out,&n);assert(n==6&&out[2]==10);
    tn_init(&t,1);t.used=t.sent=0;
    assert(tn_data(&t,(const unsigned char *)"A\r\nB\r\377",6));
    assert(t.used==sizeof(encoded)&&!memcmp(t.wire,encoded,sizeof(encoded)));
    tn_init(&t,0);n=0;
    feed(&t,(const unsigned char *)"\377\375\003",3,out,&n);
    before=t.used;feed(&t,(const unsigned char *)"\377\375\003",3,out,&n);assert(t.used==before);
    feed(&t,(const unsigned char *)"\377\375\143",3,out,&n);
    assert(t.wire[t.used-3]==255&&t.wire[t.used-2]==252&&t.wire[t.used-1]==99);
    t.used=1010;t.state=2;t.command=TN_DO;
    assert(!tn_feed(&t,3,out,&n,256)&&t.state==2);
    t.sent=20;assert(tn_feed(&t,3,out,&n,256)&&t.state==0);
    return 0;
}
