/* DEVICE_HOST_RANDOM ABI1: output copied only after a complete native fill. */
#include "entropy.h"
static int entropy_span(U p,U n,size_t *at) {
    U bank=p>>16,offset=p&65535;
    if(bank>=256||offset>=BANK)return 0;
    *at=bank*BANK+offset;return *at>=32&&n<=CELLS-*at;
}
static void entropy_device(Context *c) {
    U p=((U)c->desc_bank<<16)|c->desc_offset;size_t at,out;
    if(!entropy_span(p,2,&at)){c->device_status=4;return;}
    uint16_t *d=c->cells+at;
    if(d[0]!=1){c->device_status=2;return;}
    if(d[1]!=6){c->device_status=3;return;}
    if(!entropy_span(p,6,&at)){c->device_status=4;return;}
    if(!d[4]||d[4]>256){c->device_status=3;return;}
    if(d[5]){c->device_status=5;return;}
    if(!entropy_span(((U)d[2]<<16)|d[3],d[4],&out)||(out<at+6&&at<out+d[4])){c->device_status=4;return;}
    uint8_t bytes[256];
    if(!entropy_fill(bytes,d[4])){c->device_status=10;return;}
    dirty(c,out,d[4]);for(unsigned i=0;i<d[4];i++)c->cells[out+i]=bytes[i];
    volatile uint8_t *v=bytes;for(unsigned i=0;i<sizeof(bytes);i++)v[i]=0;
    c->device_status=0;
}
