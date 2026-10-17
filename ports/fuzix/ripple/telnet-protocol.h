/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPPLE_TELNET_PROTOCOL_H
#define RIPPLE_TELNET_PROTOCOL_H
#include <string.h>
/* Bounded streaming Telnet/NVT codec. No packet-boundary assumptions. */
#define TN_IAC 255
#define TN_SB 250
#define TN_SE 240
#define TN_WILL 251
#define TN_WONT 252
#define TN_DO 253
#define TN_DONT 254
struct telnet {
    unsigned server,state,command,option,subused,suboverflow,cr,width,height;
    unsigned char local[32],remote[32],sub[64],wire[1024];
    unsigned used,sent;
};
static unsigned tn_space(struct telnet *t) {
    if(t->sent) {memmove(t->wire,t->wire+t->sent,t->used-t->sent);t->used-=t->sent;t->sent=0;}
    return 1024-t->used;
}
static int tn_command(struct telnet *t,unsigned command,unsigned option) {
    if(tn_space(t)<3)return 0;
    t->wire[t->used++]=TN_IAC;t->wire[t->used++]=command;t->wire[t->used++]=option;return 1;
}
static void tn_init(struct telnet *t,unsigned server) {
    memset(t,0,sizeof(*t));t->server=server;t->width=80;t->height=24;
    if(server) {
        tn_command(t,TN_WILL,1);t->local[1]=1;
        tn_command(t,TN_WILL,3);t->local[3]=1;
        tn_command(t,TN_DO,31);t->remote[31]=1;
    }
}
static void tn_window(struct telnet *t,unsigned width,unsigned height) {
    unsigned char data[4];unsigned i;
    if(tn_space(t)<13)return;
    data[0]=width>>8;data[1]=width&255;data[2]=height>>8;data[3]=height&255;
    t->wire[t->used++]=TN_IAC;t->wire[t->used++]=TN_SB;t->wire[t->used++]=31;
    for(i=0;i<4;i++){t->wire[t->used++]=data[i];if(data[i]==TN_IAC)t->wire[t->used++]=TN_IAC;}
    t->wire[t->used++]=TN_IAC;t->wire[t->used++]=TN_SE;
}
static int tn_data(struct telnet *t,const unsigned char *p,unsigned n) {
    unsigned i,c;
    if(tn_space(t)<n*2)return 0;
    for(i=0;i<n;i++) {
        c=p[i]&255;t->wire[t->used++]=c;
        if(c==TN_IAC)t->wire[t->used++]=c;
        else if(c==13&&(i+1==n||p[i+1]!=10))t->wire[t->used++]=0;
    }
    return 1;
}
/* Return 0 only for backpressure. The caller must retain and retry this octet. */
static int tn_feed(struct telnet *t,unsigned c,unsigned char *out,unsigned *n,unsigned capacity) {
    unsigned allowed,old;
    if(t->state==0) {
        if(c==TN_IAC){t->state=1;return 1;}
        if(t->cr&&(c==0||(t->server&&c==10))){t->cr=0;return 1;}
        if(*n==capacity)return 0;
        out[(*n)++]=c;t->cr=c==13;return 1;
    }
    if(t->state==1) {
        if(c==TN_IAC){if(*n==capacity)return 0;out[(*n)++]=c;t->state=0;t->cr=0;return 1;}
        if(c>=TN_WILL&&c<=TN_DONT){t->command=c;t->state=2;return 1;}
        if(c==TN_SB){t->state=3;t->subused=t->suboverflow=0;return 1;}
        if(c==244||c==243){if(*n==capacity)return 0;out[(*n)++]=3;}
        t->state=0;return 1;
    }
    if(t->state==2) {
        if(tn_space(t)<20)return 0;
        if(t->command==TN_DO) {
            allowed=c==3||(t->server&&c==1)||(!t->server&&(c==31||c==24));
            old=c<32?t->local[c]:0;
            if(allowed) {
                if(!old)tn_command(t,TN_WILL,c);
                if(c<32)t->local[c]=1;
                if(!t->server&&c==31)tn_window(t,t->width,t->height);
            } else {tn_command(t,TN_WONT,c);if(c<32)t->local[c]=2;}
        } else if(t->command==TN_WILL) {
            allowed=c==3||(!t->server&&c==1)||(t->server&&c==31);
            old=c<32?t->remote[c]:0;
            if(allowed){if(!old)tn_command(t,TN_DO,c);if(c<32)t->remote[c]=1;}
            else {tn_command(t,TN_DONT,c);if(c<32)t->remote[c]=2;}
        } else if(t->command==TN_DONT) {
            if(c<32&&t->local[c]==1)tn_command(t,TN_WONT,c);
            if(c<32)t->local[c]=0;
        } else if(c<32)t->remote[c]=0;
        t->state=0;return 1;
    }
    if(t->state==3){t->option=c;t->state=4;return 1;}
    if(t->state==4) {
        if(c==TN_IAC){t->state=5;return 1;}
        if(t->subused<64)t->sub[t->subused++]=c;else t->suboverflow=1;
        return 1;
    }
    if(c==TN_IAC) {
        if(t->subused<64)t->sub[t->subused++]=c;else t->suboverflow=1;
        t->state=4;return 1;
    }
    if(c==TN_SE) {
        if(!t->suboverflow&&t->option==31&&t->server&&t->remote[31]==1&&t->subused==4) {
            unsigned width=(t->sub[0]<<8)|t->sub[1],height=(t->sub[2]<<8)|t->sub[3];
            if(width>=20&&width<=400&&height>=5&&height<=200){t->width=width;t->height=height;}
        }
        if(!t->suboverflow&&t->option==24&&!t->server&&t->local[24]==1&&t->subused==1&&t->sub[0]==1) {
            static const unsigned char reply[]={255,250,24,0,'a','n','s','i',255,240};
            if(tn_space(t)<sizeof(reply))return 0;
            memcpy(t->wire+t->used,reply,sizeof(reply));t->used+=sizeof(reply);
        }
        t->state=0;return 1;
    }
    t->state=4;return 1;
}
#endif
