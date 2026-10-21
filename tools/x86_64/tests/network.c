/* Independent host oracle for C16 DEVICE_NET_* validation and atomicity. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define RIPLIX_X86_KERNEL_H
#define BANK 64000U
#define CELLS (256U * BANK)
typedef uint64_t U;
typedef struct {uint16_t cells[CELLS],desc_bank,desc_offset,device_status;} Context;
static Context context;
static unsigned calls,delivered,dirty_count;
static size_t last_dirty,last_size;
static unsigned queued=1;
static void dirty(Context *c,size_t at,size_t count){(void)c;++dirty_count;last_dirty=at;last_size=count;}
#include "../boot/net.h"
void nic_info(NicInfo *out){++calls;*out=(NicInfo){.link=1,.rx_frames=queued,.counters={42}};}
unsigned nic_tx(const uint8_t *p,unsigned n){++calls;assert(n==14&&p[0]==7&&p[13]==8);return NIC_OK;}
unsigned nic_rx(uint8_t *p,unsigned n,unsigned *length){
    ++calls;*length=0;if(!queued)return NIC_EMPTY;*length=20;
    if(n<20)return NIC_TOO_SMALL;
    for(unsigned i=0;i<20;i++)p[i]=(uint8_t)(i+230);
    queued=0;++delivered;return NIC_OK;
}
#include "../boot/network.inc"
static uint16_t *descriptor(unsigned length){
    Context *c=&context;c->desc_bank=1;c->desc_offset=100;
    uint16_t *d=c->cells+BANK+100;memset(d,0,54);d[0]=1;d[1]=length;
    d[3]=2;d[4]=63995;d[5]=20;return d;
}
static void rejected(unsigned command,unsigned status,unsigned before){
    unsigned was_dirty=dirty_count;network_device(&context,command);
    assert(context.device_status==status&&calls==before&&dirty_count==was_dirty&&queued==1);
}
int main(void){
    Context *c=&context;uint16_t *d;unsigned before;
    d=descriptor(18);network_device(c,0x100);assert(!c->device_status&&d[3]==1&&d[4]==1500&&d[5]==1514);
    assert(d[12]==64&&d[13]==64&&d[14]==0&&d[15]==1&&d[16]==0&&d[17]==1);
    d=descriptor(27);network_device(c,0x103);assert(!c->device_status&&d[9]==42);
    for(unsigned cmd=0x100;cmd<=0x103;cmd++){
        unsigned len=cmd==0x100?18:cmd==0x101?8:cmd==0x102?9:27;
        before=calls;d=descriptor(len);d[0]=2;rejected(cmd,2,before);
        d=descriptor(len);d[1]=len-1;rejected(cmd,3,before);
        d=descriptor(len);d[2]=1;rejected(cmd,5,before);
        d=descriptor(len);c->desc_bank=256;rejected(cmd,4,before);
        d=descriptor(len);c->desc_offset=64000;rejected(cmd,4,before);
        d=descriptor(len);c->desc_bank=255;c->desc_offset=63999;rejected(cmd,4,before);
    }
    for(unsigned cmd=0x101;cmd<=0x102;cmd++){
        unsigned len=cmd==0x101?8:9;
        before=calls;d=descriptor(len);d[len-1]=1;rejected(cmd,5,before);
        d=descriptor(len);d[3]=256;rejected(cmd,4,before);
        d=descriptor(len);d[4]=64000;rejected(cmd,4,before);
        d=descriptor(len);d[3]=255;d[4]=63995;rejected(cmd,4,before);
        d=descriptor(len);d[3]=1;d[4]=102;rejected(cmd,4,before);
        d=descriptor(len);d[3]=0;d[4]=27;rejected(cmd,4,before);
    }
    before=calls;d=descriptor(8);d[5]=13;rejected(0x101,3,before);
    d=descriptor(8);d[5]=1515;rejected(0x101,3,before);
    d=descriptor(8);d[5]=14;c->cells[3*BANK-5]=256;rejected(0x101,3,before);
    for(unsigned i=0;i<20;i++)c->cells[3*BANK-5+i]=0xabcd;
    d=descriptor(9);d[5]=19;network_device(c,0x102);
    assert(!c->device_status&&d[6]==20&&d[7]==NIC_TOO_SMALL&&queued==1&&!delivered);
    for(unsigned i=0;i<20;i++)assert(c->cells[3*BANK-5+i]==0xabcd);
    d=descriptor(9);network_device(c,0x102);
    assert(!c->device_status&&d[6]==20&&d[7]==NIC_OK&&!queued&&delivered==1);
    assert(last_dirty==BANK+100&&last_size==9);
    for(unsigned i=0;i<20;i++)assert(c->cells[3*BANK-5+i]==i+230);
    d=descriptor(9);network_device(c,0x102);assert(!d[6]&&d[7]==NIC_EMPTY);
    d=descriptor(8);d[5]=14;c->cells[3*BANK-5]=7;c->cells[3*BANK+8]=8;
    network_device(c,0x101);assert(!c->device_status&&d[6]==NIC_OK);
    puts("PASS: network mailbox bounds, malformed descriptors, bank crossings, RX retention and octet atomicity");
}
