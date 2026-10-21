/* See wifi.h and wifi-LICENSE.txt. Untrusted TLVs parsed without packed casts. */
#include "wifi.h"
static uint32_t get32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
const char *wifi_image_parse(const uint8_t *data, size_t bytes, WifiImage *image) {
    *image=(WifiImage){0};
    if(bytes<88 || bytes>16*1024*1024 || get32(data) || get32(data+4)!=0x0a4c5749)
        return "firmware header/size";
    unsigned group=0, versions=0, commands=0, records=0, alive=0;
    uint32_t capabilities=0,apis=0;
    for(size_t at=88;at<bytes;) {
        if(bytes-at<8 || ++records>65536) return "firmware TLV header/count";
        uint32_t type=get32(data+at),size=get32(data+at+4); at+=8;
        size_t padded=((size_t)size+3)&~(size_t)3;
        if(padded>bytes-at) return "firmware TLV bounds";
        const uint8_t *p=data+at;
        if(type==19) {
            if(size<4) return "firmware section header";
            uint32_t offset=get32(p);
            if(offset==0xffffcccc || offset==0xaaaabbbb) {
                if(size!=8 || group>1 || !image->count[group] ||
                   offset!=(group==0?0xffffccccU:0xaaaabbbbU)) return "firmware section separator";
                ++group;
            } else {
                if(size<=4 || size>32772 || image->count[group]==64) return "firmware section size/count";
                image->section[group][image->count[group]++]=(WifiSection){p+4,size-4};
            }
        } else if(type==30) {
            if(size!=8 || get32(p)>=8)return "firmware capability format";
            unsigned index=get32(p);
            if(capabilities&(1U<<index))return "firmware duplicate capability";
            capabilities|=1U<<index;image->capabilities[index]=get32(p+4);
            if(index==1)image->fragmented_pnvm=get32(p+4)&1;
        } else if(type==29) {
            if(size!=8 || get32(p)>=8)return "firmware API format";
            unsigned index=get32(p);
            if(apis&(1U<<index))return "firmware duplicate API";
            apis|=1U<<index;image->api[index]=get32(p+4);
        } else if(type==52) {
            if(image->iml.data || !size || size>256*1024) return "firmware IML size/duplicate";
            image->iml=(WifiSection){p,size};
        } else if(type==36) {
            if(++versions!=1 || size!=12) return "firmware version record";
            for(unsigned n=0;n<3;n++) image->version[n]=get32(p+4*n);
        } else if(type==48) {
            if(++commands!=1 || size%4) return "firmware command versions";
            for(unsigned n=0;n<size;n+=4) {
                if(p[n]==0xfe && p[n+1]==12)image->pnvm_notification=p[n+3];
                if(p[n]==2 && p[n+1]==12)image->nvm_notification=p[n+3];
                if(p[n]==12 && p[n+1]==1)image->scan_cfg_version=p[n+2];
                if(p[n]==13 && p[n+1]==1)image->scan_request_version=p[n+2];
                if(p[n]==193 && p[n+1]==0)image->rx_notification=p[n+3];
                if(p[n]==200 && p[n+1]==1)image->mcc_notification=p[n+3];
                if(p[n]==8 && p[n+1]==1)image->phy_version=p[n+2];
                if(p[n]==8 && p[n+1]==5)image->rlc_version=p[n+2];
                if(p[n]==8 && p[n+1]==3)image->mac_version=p[n+2];
                if(p[n]==9 && p[n+1]==3)image->link_version=p[n+2];
                if(p[n]==10 && p[n+1]==3)image->sta_version=p[n+2];
                if(p[n]==23 && p[n+1]==5)image->queue_version=p[n+2];
                if(p[n]==24 && p[n+1]==5)image->key_version=p[n+2];
                if(p[n]==0xa2 && p[n+1]==0)image->beacon_notification=p[n+3];
                if(p[n]==28 && p[n+1]==1){image->tx_version=p[n+2];image->tx_notification=p[n+3];}
                if(p[n]==5 && p[n+1]==3)image->session_version=p[n+2];
                if(p[n]==251 && p[n+1]==3)image->session_notification=p[n+3];
                if(p[n]!=1 || p[n+1]!=0)continue;
                if(++alive!=1 || p[n+3]<5 || p[n+3]>7) return "firmware ALIVE version";
                image->alive_version=p[n+3];
            }
        }
        at+=padded;
    }
    if(group!=2 || !image->count[2] || !image->iml.data || versions!=1 ||
       image->version[0]!=89 || alive!=1) return "firmware missing/unsupported startup records";
    return 0;
}
static void put16(uint8_t *p,unsigned v){p[0]=v;p[1]=v>>8;}
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=v>>(8*i);}
static void put64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=v>>(8*i);}
void *wifi_dma_reserve(WifiBoot *b,unsigned size) {
    if(size>UINT32_MAX-4095 || b->used>b->capacity)return 0;
    unsigned padded=(size+4095)&~4095U;
    if(!padded || padded>b->capacity-b->used) return 0;
    uint8_t *p=b->pool+b->used; b->used+=padded;
    for(unsigned i=0;i<padded;i++)p[i]=0;
    return p;
}
void wifi_rx_descriptors(WifiBoot *b) {
    for(unsigned n=0;n<WIFI_RX_COUNT;n++) {
        put16(b->free_ring+n*16,n);
        put64(b->free_ring+n*16+8,(uintptr_t)b->rx+n*4096);
    }
}
const char *wifi_layout(WifiBoot *b,const WifiImage *fw,uint16_t revision) {
    if(b->attempted || !b->pool || (uintptr_t)b->pool%4096 || b->used || b->capacity<WIFI_DMA_BYTES)
        return "DMA pool state/alignment/size";
    /* Page alignment exceeds every upstream ring/context alignment requirement. */
    b->context=wifi_dma_reserve(b,WIFI_CONTEXT_BYTES); b->scratch=wifi_dma_reserve(b,WIFI_SCRATCH_BYTES);
    b->info=wifi_dma_reserve(b,4096); b->status=wifi_dma_reserve(b,4096);
    b->free_ring=wifi_dma_reserve(b,WIFI_RX_COUNT*16); b->used_ring=wifi_dma_reserve(b,WIFI_RX_COUNT*32);
    b->rx=wifi_dma_reserve(b,WIFI_RX_COUNT*4096); b->tx=wifi_dma_reserve(b,256*256);
    b->iml=wifi_dma_reserve(b,fw->iml.size);
    if(!b->iml) return "DMA base allocation";
    for(unsigned i=0;i<fw->iml.size;i++)b->iml[i]=fw->iml.data[i];
    uint8_t *c=b->context,*s=b->scratch;
    put16(s,revision); put16(s+4,WIFI_SCRATCH_BYTES/4); put32(s+8,0xf0000);
    put64(s+48,(uintptr_t)b->free_ring);
    for(unsigned group=0;group<3;group++) {
        if(!fw->count[group] || fw->count[group]>64) return "DMA section count";
        for(unsigned n=0;n<fw->count[group];n++) {
            WifiSection section=fw->section[group][n];
            if(!section.size || section.size>32768) return "DMA section size";
            uint8_t *dest=wifi_dma_reserve(b,section.size);
            if(!dest) return "DMA section allocation";
            for(unsigned i=0;i<section.size;i++)dest[i]=section.data[i];
            /* DRAM ABI is UMAC, LMAC, paging; parser order is LMAC, UMAC, paging. */
            unsigned slot=group<2?1-group:2;
            put64(s+124+slot*512+n*8,(uintptr_t)dest);
        }
    }
    wifi_rx_descriptors(b);
    for(unsigned n=0;n<WIFI_RX_COUNT;n++)put32(b->rx+n*4096,0x55550000);
    put64(c+8,(uintptr_t)b->info); put64(c+16,(uintptr_t)b->status);
    put64(c+24,(uintptr_t)b->info+2048); put64(c+32,(uintptr_t)b->info+3072);
    put64(c+52,(uintptr_t)b->tx); put64(c+60,(uintptr_t)b->used_ring);
    put16(c+68,5); put16(c+70,9);
    put64(c+88,(uintptr_t)s); put32(c+96,WIFI_SCRATCH_BYTES);
    return 0;
}
const char *wifi_alive_packet(WifiBoot *b,const uint8_t *p,size_t bytes,unsigned version) {
    if(bytes<8) return "RX short header";
    uint32_t flags=get32(p),size=flags&0x3fff;
    if(flags==0x55550000 || size<4 || size>bytes-4) return "RX packet bounds";
    if(p[4]!=1 || p[5]!=0) return 0;
    if(!(p[7]&0x80) || version<5 || version>7 || size-4!=(version==5?128:144))
        return "ALIVE notification format";
    if((p[8]|(unsigned)p[9]<<8)!=0xcafe) return "ALIVE firmware status";
    b->lmac_major=get32(p+12); b->lmac_minor=get32(p+16);
    b->umac_major=get32(p+108); b->umac_minor=get32(p+112);
    for(unsigned i=0;i<3;i++)b->sku[i]=get32(p+124+4*i);
    b->alive=1;
    return 0;
}
