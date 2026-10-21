/* Public PNVM TLV/SKU layout and DMA handoff, from OpenBSD iwx.
 * See wifi-LICENSE.txt. Board data stays opaque; no radio commands here. */
#include "wifi.h"
static uint32_t get32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static unsigned get16(const uint8_t *p){return p[0]|(unsigned)p[1]<<8;}
static void put32(uint8_t *p,uint32_t v){for(unsigned n=0;n<4;n++)p[n]=v>>(8*n);}
static void put64(uint8_t *p,uint64_t v){for(unsigned n=0;n<8;n++)p[n]=v>>(8*n);}
const char *wifi_pnvm_select(const uint8_t *data,size_t bytes,const uint32_t sku[3],unsigned mac,unsigned rf,WifiPnvm *out) {
    *out=(WifiPnvm){0};
    if(!data || !bytes || bytes>1024*1024)return "PNVM container size";
    unsigned matched=0,hw=0,version=0,seen=0,records=0;
    for(size_t at=0;at<bytes;) {
        if(bytes-at<8 || ++records>8192)return "PNVM TLV header/count";
        uint32_t type=get32(data+at),size=get32(data+at+4);at+=8;
        size_t padded=((size_t)size+3)&~(size_t)3;
        if(padded>bytes-at)return "PNVM TLV bounds";
        const uint8_t *p=data+at;
        if(type==64) {
            if(size!=12)return "PNVM SKU format";
            unsigned matches=1;
            for(unsigned n=0;n<3;n++)if(get32(p+4*n)!=sku[n])matches=0;
            if(matches && ++seen!=1)return "PNVM duplicate SKU";
            matched=matches;
        } else if(type==58) {
            if(size<4)return "PNVM HW format";
            if(matched) {
                if(hw++)return "PNVM duplicate HW record";
                if(get16(p)!=mac || get16(p+2)!=rf)return "PNVM HW mismatch";
            }
        } else if(type==62) {
            if(size!=4)return "PNVM version format";
            if(matched) {
                if(version++)return "PNVM duplicate version";
                out->version=get32(p);
            }
        } else if(type==19) {
            if(size<4)return "PNVM section header";
            if(matched && get32(p)!=0xddddeeee) {
                if(size==4 || size>65540 || out->count==64)return "PNVM section size/count";
                out->section[out->count++]=(WifiSection){p+4,size-4};out->size+=size-4;
            }
        }
        at+=padded;
    }
    if(!seen || !hw || !version || !out->count)return "PNVM matching SKU/HW/data missing";
    return 0;
}
const char *wifi_pnvm_layout(WifiBoot *b,const WifiImage *fw) {
    if(!b->alive || b->pnvm_started || !b->scratch)return "PNVM startup state";
    WifiPnvm selected;
    const char *error=wifi_pnvm_select(fw->pnvm.data,fw->pnvm.size,b->sku,0x44,0x10d,&selected);
    if(error)return error;
    uint8_t *base=wifi_dma_reserve(b,fw->fragmented_pnvm?512:selected.size);
    if(!base)return "PNVM DMA allocation";
    unsigned at=0;
    for(unsigned n=0;n<selected.count;n++) {
        WifiSection section=selected.section[n];
        uint8_t *dest=fw->fragmented_pnvm?wifi_dma_reserve(b,section.size):base+at;
        if(!dest)return "PNVM fragment allocation";
        for(unsigned i=0;i<section.size;i++)dest[i]=section.data[i];
        if(fw->fragmented_pnvm)put64(base+8*n,(uintptr_t)dest);
        at+=section.size;
    }
    /* Firmware expects bytes here despite the DW wording in ABI comments. */
    put64(b->scratch+16,(uintptr_t)base);put32(b->scratch+24,selected.size);
    b->pnvm_version=selected.version;b->pnvm_size=selected.size;b->pnvm_count=selected.count;
    return 0;
}

/* PNVM_INIT_COMPLETE_NTFY_S_VER_1: public firmware ABI, status is LE u32.
 * Linux/OpenBSD proceed on this notification, without requiring status zero.
 * Preserve the raw value; notification receipt alone does not prove board setup. */
const char *wifi_pnvm_packet(WifiBoot *b,const uint8_t *p,size_t bytes) {
    if(bytes<8)return "PNVM RX header";
    unsigned size=get32(p)&0x3fff;
    if(size<4 || size>bytes-4)return "PNVM RX bounds";
    if(p[4]!=0xfe || p[5]!=12)return 0;
    if(!b->pnvm_started || b->pnvm_complete || !(p[7]&0x80) || size!=8)return "PNVM notification format/state";
    b->pnvm_status=get32(p+8);
    b->pnvm_complete=1;
    return 0;
}
