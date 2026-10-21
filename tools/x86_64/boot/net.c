/* Modern virtio-net 1.x; polled split queues, no offload or mergeable RX.
 * Native permanent DMA buffers isolate the C16 memory/continuation ABI.
 * Reference: Virtio 1.2 sections 2.7, 4.1, 5.1. Header is TWELVE octets
 * even without MRG_RXBUF (ten octets applies only to legacy devices).
 * No device IRQ runs guest code; the existing kernel network poll owns I/O. */
#include "net.h"
#include "kernel.h"
#include "pci.h"
#include "dma.h"
#define HEADER 12U
#define WIRE (HEADER + NIC_FRAME)
typedef struct { uint64_t address; uint32_t length; uint16_t flags, next; } NetDesc;
typedef struct {
    NetDesc desc[NIC_FRAMES];
    struct { uint16_t flags, index, ring[NIC_FRAMES], event; } available;
    struct { uint16_t flags, index; struct { uint32_t id, length; } ring[NIC_FRAMES]; uint16_t event; } used;
} NetQueue;
_Static_assert(sizeof(NetDesc)==16, "virtio descriptor size");
_Static_assert(__builtin_offsetof(NetQueue,used)%4==0, "virtio used alignment");
_Static_assert(sizeof(NetQueue)%16==0, "virtio descriptor array alignment");
static NetQueue queues[2] __attribute__((aligned(4096)));
static uint8_t buffers[2][NIC_FRAMES][WIRE] __attribute__((aligned(4096)));
static uint8_t received[NIC_FRAMES][NIC_FRAME];
static unsigned lengths[NIC_FRAMES], tx_lengths[NIC_FRAMES];
static uint8_t owned[2][NIC_FRAMES];
static unsigned head, count, octets, tx_count, tx_octets, active;
static uint16_t available[2], completed[2];
static NicInfo info;
static volatile uint8_t *common, *configuration;
static volatile uint16_t *notify[2];
static void fence(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static uint16_t read16(unsigned n) { return *(volatile uint16_t *)(common + n); }
static uint32_t read32(unsigned n) { return *(volatile uint32_t *)(common + n); }
static void write16(unsigned n, uint16_t v) { *(volatile uint16_t *)(common + n) = v; }
static void write32(unsigned n, uint32_t v) { *(volatile uint32_t *)(common + n) = v; }
static void write64(unsigned n, uint64_t v) { write32(n,v); write32(n+4,v>>32); }
static void increment(unsigned n) { if (info.counters[n] != UINT32_MAX) ++info.counters[n]; }
static void broken(const char *why) {
    if (active) {
        active = info.link = 0;
        increment(8);
        for (unsigned i=0;i<tx_count;++i) increment(5);
        tx_count=tx_octets=0;
        common[20] |= 128;
        serial("NETWORK_DEVICE_ERROR "); serial(why); serial("\n");
    }
    /* DMA storage stays resident, including after reset/failure. */
}
static void publish(unsigned q, unsigned id) {
    queues[q].available.ring[available[q] % NIC_FRAMES] = id;
    owned[q][id] = 1;
    fence(); queues[q].available.index = ++available[q]; fence();
}
static void poll(void) {
    if (!active) return;
    if (common[20] & (64|128)) { broken("device reset required"); return; }
    info.link = !!(*(volatile uint16_t *)(configuration+6) & 1);
    for (unsigned q=0;q<2;++q) {
        volatile uint16_t *index=&queues[q].used.index;
        uint16_t limit=*index;
        if ((uint16_t)(limit-completed[q])>NIC_FRAMES) { broken("used index"); return; }
        while(completed[q]!=limit) {
            fence();
            unsigned slot=completed[q] % NIC_FRAMES;
            uint32_t id=queues[q].used.ring[slot].id, n=queues[q].used.ring[slot].length;
            if(id>=NIC_FRAMES || !owned[q][id]) { broken("used descriptor"); return; }
            owned[q][id]=0; ++completed[q];
            if(q) { /* Used length is ignored for device-readable TX buffers. */
                --tx_count; tx_octets-=tx_lengths[id]; increment(4);
                continue;
            }
            increment(0);
            uint8_t *packet=buffers[0][id];
            if(n>WIRE) { increment(7); increment(2); }
            /* No mergeable buffers negotiated: exactly one descriptor owns
             * the frame. QEMU leaves num_buffers zero in this mode; accept
             * zero/one, never interpret it as a continuation descriptor. */
            else if(n<HEADER+14 || packet[0] || packet[1] || packet[10]>1 || packet[11]) increment(2);
            else {
                unsigned bytes=n-HEADER;
                if(count==NIC_FRAMES || bytes>NIC_OCTETS-octets) increment(2);
                else {
                    unsigned tail=(head+count)%NIC_FRAMES;
                    memcpy(received[tail],packet+HEADER,bytes);
                    lengths[tail]=bytes; ++count; octets+=bytes;
                }
            }
            publish(0,id);
        }
        if(!q) *notify[0]=0;
    }
}
void nic_init(void) {
    int found=pci_find(0x1af4,0x1041);
    if(found<0) { serial("NETWORK_NO_DEVICE virtio-net absent; link down\n"); return; }
    unsigned bdf=found;
    const char *error=dma_identity_check(bdf);
    if(error) fail(error,bdf);
    uint16_t command=pci_read(bdf,4);
    pci_write16(bdf,4,command&~4U); /* No DMA while replacing firmware queues. */
    uint64_t bars[6],sizes[6],address[5]={0},length[5]={0};
    pci_bars(bdf,bars,sizes);
    pci_write16(bdf,4,(command|2|0x400)&~4U); /* MMIO on, DMA and INTx off. */
    if(!(pci_read(bdf,4)&(1U<<20))) fail("network PCI capabilities",bdf);
    uint8_t seen[256]={0}; uint32_t multiplier=0;
    for(unsigned cap=pci_read(bdf,52)&255;cap;) {
        if(cap<64 || cap>252 || (cap&3) || seen[cap]) fail("network PCI capability chain",cap);
        seen[cap]=1;
        uint32_t h=pci_read(bdf,cap); unsigned id=h&255,type=h>>24,n=h>>16&255;
        if(id==0x11) pci_write16(bdf,cap+2,(h>>16&~0x8000U)|0x4000);
        else if(id==5) pci_write16(bdf,cap+2,h>>16&~1U);
        else if(id==9 && type>=1 && type<=4 && !address[type]) {
            if(n<16 || cap+n>256 || (type==2&&n<20)) fail("network virtio capability length",cap);
            unsigned bar=pci_read(bdf,cap+4)&255;
            if(bar<6) {
                uint64_t off=pci_read(bdf,cap+8),len=pci_read(bdf,cap+12);
                if(!bars[bar] || off>sizes[bar] || len>sizes[bar]-off ||
                   off>UINT64_MAX-bars[bar] || len>UINT64_MAX-bars[bar]-off)
                    fail("network virtio BAR bounds",cap);
                address[type]=bars[bar]+off; length[type]=len;
                if(type==2)multiplier=pci_read(bdf,cap+16);
            }
        }
        cap=h>>8&255;
    }
    if(!address[1] || length[1]<56 || (address[1]&3) || !address[2] || length[2]<2 ||
       !address[4] || length[4]<8 || (address[4]&1))fail("network virtio MMIO regions",bdf);
    common=device_map(address[1],56); configuration=device_map(address[4],8);
    common[20]=0;
    uint64_t deadline=rdtsc()+boot.tsc_hz;
    while(common[20])if(rdtsc()>=deadline)fail("network reset timeout",bdf);
    common[20]=1; common[20]=3;
    write32(0,1);uint32_t high=read32(4);
    write32(0,0);uint32_t low=read32(4);
    unsigned features=(1U<<5)|(1U<<16); /* MAC and link STATUS required. */
    if(!(high&1) || (low&features)!=features)fail("network required features",low);
    write32(8,0);write32(12,features);write32(8,1);write32(12,1);
    common[20]=11;
    if(!(common[20]&8))fail("network features rejected",common[20]);
    for(unsigned attempt=0;;++attempt) {
        uint8_t generation=common[21];
        for(unsigned i=0;i<6;++i)info.mac[i]=configuration[i];
        if(generation==common[21])break;
        if(attempt==99)fail("network configuration unstable",bdf);
    }
    unsigned nonzero=0;for(unsigned i=0;i<6;++i)nonzero|=info.mac[i];
    if(!nonzero || (info.mac[0]&1))fail("network invalid unicast MAC",bdf);
    write16(16,65535);
    for(unsigned q=0;q<2;++q) {
        write16(22,q);
        if(read16(24)<NIC_FRAMES || read16(28))fail("network queue unavailable",q);
        write16(24,NIC_FRAMES);write16(26,65535);
        queues[q].available.flags=1;
        write64(32,(uintptr_t)queues[q].desc);
        write64(40,(uintptr_t)&queues[q].available);
        write64(48,(uintptr_t)&queues[q].used);
        uint64_t off=(uint64_t)read16(30)*multiplier;
        if(off>length[2]-2 || ((address[2]+off)&1))fail("network notify bounds",off);
        notify[q]=device_map(address[2]+off,2);
        for(unsigned i=0;i<NIC_FRAMES;++i) {
            queues[q].desc[i]=(NetDesc){(uintptr_t)buffers[q][i],WIRE,q?0:2,0};
            if(!q)publish(q,i);
        }
        write16(28,1);
    }
    pci_write16(bdf,4,(pci_read(bdf,4)|6|0x400)&65535);
    fence(); common[20]=15; active=1; *notify[0]=0;
    poll();serial("VIRTIO_NET_OK pci=");hex(bdf);serial(" link=");hex(info.link);serial("\n");
}
void nic_info(NicInfo *out) {
    poll();*out=info;out->rx_frames=count;out->rx_octets=octets;
    out->tx_frames=tx_count;out->tx_octets=tx_octets;
}
unsigned nic_tx(const uint8_t *data,unsigned n) {
    poll();if(!active || !info.link)return NIC_DOWN;
    if(n<14 || n>NIC_FRAME) return NIC_DOWN; /* Mailbox rejects invalid lengths first. */
    if(tx_count==NIC_FRAMES || n>NIC_OCTETS-tx_octets){increment(6);return NIC_WOULD_BLOCK;}
    unsigned id=0;while(id<NIC_FRAMES&&owned[1][id])++id;
    if(id==NIC_FRAMES){broken("TX ownership");return NIC_DOWN;}
    memset(buffers[1][id],0,HEADER);memcpy(buffers[1][id]+HEADER,data,n);
    queues[1].desc[id].length=HEADER+n;tx_lengths[id]=n;
    ++tx_count;tx_octets+=n;increment(3);publish(1,id);*notify[1]=1;
    return NIC_OK;
}
unsigned nic_rx(uint8_t *data,unsigned capacity,unsigned *length) {
    poll();*length=0;
    if(!count)return active&&info.link?NIC_EMPTY:NIC_DOWN;
    *length=lengths[head];
    if(capacity<*length)return NIC_TOO_SMALL;
    memcpy(data,received[head],*length);octets-=*length;
    head=(head+1)%NIC_FRAMES;--count;increment(1);return NIC_OK;
}
void nic_stop(void) {
    if(!active)return;
    common[20]=0;
    uint64_t deadline=rdtsc()+boot.tsc_hz;
    while(common[20])if(rdtsc()>=deadline)fail("network stop timeout",0);
    active=info.link=0;
    serial("NETWORK_QUIESCED\n");
}
