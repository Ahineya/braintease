/* Production virtio completion paths with controlled native DMA buffers. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define RIPLIX_X86_KERNEL_H
#include "../boot/bootinfo.h"
BootInfo boot;
static void serial(const char *s){(void)s;}
static void hex(uint64_t n){(void)n;}
static uint64_t rdtsc(void){static uint64_t n;return ++n;}
static _Noreturn void fail(const char *s,uint64_t n){fprintf(stderr,"%s %llu\n",s,(unsigned long long)n);abort();}
static void *device_map(uint64_t p,size_t n){(void)n;return(void *)(uintptr_t)p;}
#include "../boot/net.c"
uint32_t pci_read(unsigned b,unsigned n){(void)b;(void)n;return 0;}
void pci_write(unsigned b,unsigned n,uint32_t v){(void)b;(void)n;(void)v;}
void pci_write16(unsigned b,unsigned n,uint16_t v){(void)b;(void)n;(void)v;}
int pci_find(unsigned v,unsigned d){(void)v;(void)d;return -1;}
void pci_bars(unsigned b,uint64_t a[6],uint64_t s[6]){(void)b;(void)a;(void)s;}
const char *dma_identity_check(unsigned b){(void)b;return NULL;}
static uint32_t registers[64];static uint16_t config[4],notifications[2];
static void reset(void){
    memset(queues,0,sizeof(queues));memset(buffers,0,sizeof(buffers));memset(owned,0,sizeof(owned));
    memset(&info,0,sizeof(info));memset(available,0,sizeof(available));memset(completed,0,sizeof(completed));
    head=count=octets=tx_count=tx_octets=0;active=1;info.link=1;
    common=(void *)registers;configuration=(void *)config;common[20]=15;config[3]=1;
    notify[0]=notifications;notify[1]=notifications+1;
    for(unsigned i=0;i<NIC_FRAMES;i++)owned[0][i]=1;
}
static void receive(unsigned id,unsigned bytes,unsigned flags){
    unsigned slot=queues[0].used.index%NIC_FRAMES;
    if(id<NIC_FRAMES){
        uint8_t *p=buffers[0][id];memset(p,0,WIRE);p[0]=flags;p[10]=1;
        for(unsigned i=HEADER;i<WIRE;i++)p[i]=(uint8_t)i;
    }
    queues[0].used.ring[slot].id=id;queues[0].used.ring[slot].length=bytes;
    ++queues[0].used.index;
}
int main(void){
    _Static_assert(sizeof(NetDesc)==16,"descriptor size");
    _Static_assert(__builtin_offsetof(NetQueue,used)%4==0,"used alignment");
    _Static_assert(sizeof(NetQueue)%16==0,"array descriptor alignment");
    uint8_t data[NIC_FRAME];unsigned n;NicInfo status;
    reset();receive(7,HEADER+60,0);nic_info(&status);
    assert(status.rx_frames==1&&status.rx_octets==60&&status.counters[0]==1&&owned[0][7]);
    memset(data,0xcc,sizeof(data));assert(nic_rx(data,59,&n)==NIC_TOO_SMALL&&n==60&&count==1&&data[0]==0xcc);
    assert(nic_rx(data,sizeof(data),&n)==NIC_OK&&n==60&&!count);
    for(unsigned i=0;i<60;i++)assert(data[i]==(uint8_t)(HEADER+i));
    assert(nic_rx(data,sizeof(data),&n)==NIC_EMPTY&&n==0);
    receive(8,HEADER+60,0);buffers[0][8][10]=0;poll();
    assert(nic_rx(data,sizeof(data),&n)==NIC_OK&&n==60);
    receive(0,WIRE+1,0);receive(1,HEADER+13,0);receive(2,HEADER+60,1);poll();
    assert(info.counters[2]==3&&info.counters[7]==1&&!count&&active);
    reset();for(unsigned i=0;i<44;i++)receive(i,WIRE,0);poll();
    assert(count==43&&octets==43*NIC_FRAME&&info.counters[2]==1);
    config[3]=0;assert(nic_rx(data,sizeof(data),&n)==NIC_OK&&n==NIC_FRAME);
    while(count)assert(nic_rx(data,sizeof(data),&n)==NIC_OK);
    assert(nic_rx(data,sizeof(data),&n)==NIC_DOWN);
    reset();memset(data,0x5a,sizeof(data));assert(nic_tx(data,100)==NIC_OK&&tx_count==1&&tx_octets==100);
    assert(!memcmp(buffers[1][0]+HEADER,data,100)&&owned[1][0]);
    memset(data,0x33,100);assert(buffers[1][0][HEADER]==0x5a);
    queues[1].used.ring[0].id=0;queues[1].used.ring[0].length=UINT32_MAX;queues[1].used.index=1;
    nic_info(&status);assert(status.tx_frames==0&&status.tx_octets==0&&status.counters[4]==1);
    for(unsigned i=0;i<43;i++)assert(nic_tx(data,NIC_FRAME)==NIC_OK);
    assert(nic_tx(data,NIC_FRAME)==NIC_WOULD_BLOCK&&info.counters[6]==1);
    reset();completed[0]=65530;queues[0].used.index=65530;available[0]=65530;
    for(unsigned i=0;i<10;i++){receive(i,HEADER+14,0);poll();}
    assert(completed[0]==4&&available[0]==4&&count==10);
    reset();receive(NIC_FRAMES,HEADER+60,0);poll();assert(!active&&info.counters[8]==1);
    reset();owned[0][3]=0;receive(3,HEADER+60,0);poll();assert(!active);
    reset();queues[0].used.index=NIC_FRAMES+1;poll();assert(!active);
    reset();info.counters[0]=UINT32_MAX;receive(0,HEADER+14,0);poll();assert(info.counters[0]==UINT32_MAX);
    nic_stop();assert(!active&&!common[20]);
    puts("PASS: virtio-net RX/TX ownership, wrap, bounds, quotas, backpressure, link-down and saturating counters");
}
