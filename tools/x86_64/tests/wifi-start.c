/* The real firmware parser/layout/startup state machine, with mock MMIO/DMA.
 * This proves protocol bounds and failure handling, not physical RF support. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../boot/wifi.h"
static uint8_t pool[WIFI_DMA_BYTES] __attribute__((aligned(4096)));
static WifiImage image;
static uint8_t *blob;
static size_t blob_size;
static uint32_t u32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t u64(const uint8_t *p){return u32(p)|(uint64_t)u32(p+4)<<32;}
static void p32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=v>>(8*i);}
static void alive(uint8_t *p,unsigned version) {
    memset(p,0,4096);p32(p,4+(version==5?128:144));p[4]=1;p[7]=0x80;p[8]=0xfe;p[9]=0xca;
    p32(p+12,89);p32(p+16,0x735badc4);p32(p+108,89);p32(p+112,0x735badc4);
    for(unsigned n=0;n<3;n++)p32(p+124+n*4,n+11);
}
static void layout_test(void) {
    WifiBoot b={.pool=pool,.capacity=sizeof(pool)};
    assert(!wifi_layout(&b,&image,0x441));assert(b.used<sizeof(pool));
    assert(u32(b.scratch)==0x441 && u32(b.scratch+4)==415 && u32(b.scratch+8)==0xf0000);
    assert(u64(b.scratch+48)==(uintptr_t)b.free_ring);
    for(unsigned g=0;g<3;g++)for(unsigned n=0;n<image.count[g];n++) {
        uint64_t a=u64(b.scratch+124+(g<2?1-g:2)*512+n*8);
        assert(a>=(uintptr_t)pool && a+image.section[g][n].size<=(uintptr_t)pool+b.used && a%4096==0);
        assert(!memcmp((void *)(uintptr_t)a,image.section[g][n].data,image.section[g][n].size));
    }
    assert(u64(b.context+8)==(uintptr_t)b.info && u64(b.context+16)==(uintptr_t)b.status);
    assert(u64(b.context+24)==(uintptr_t)b.info+2048 && u64(b.context+32)==(uintptr_t)b.info+3072);
    assert(u64(b.context+52)==(uintptr_t)b.tx && u64(b.context+60)==(uintptr_t)b.used_ring);
    assert(u32(b.context+68)==0x00090005 && u64(b.context+88)==(uintptr_t)b.scratch);
    assert(u32(b.context+96)==1660);
    for(unsigned i=0;i<512;i++) {
        assert(u32(b.free_ring+i*16)==i && u32(b.free_ring+i*16+4)==0);
        assert(u64(b.free_ring+i*16+8)==(uintptr_t)b.rx+i*4096);
        assert(u32(b.rx+i*4096)==0x55550000);
    }
    assert(!memcmp(b.iml,image.iml.data,image.iml.size));
    assert(wifi_layout(&b,&image,0));
    b=(WifiBoot){.pool=pool+1,.capacity=sizeof(pool)};assert(wifi_layout(&b,&image,0));
    b=(WifiBoot){.pool=pool,.capacity=sizeof(pool)-1};assert(wifi_layout(&b,&image,0));
}
static void packet_test(void) {
    uint8_t packet[4096];WifiBoot b={0};
    for(unsigned v=5;v<=7;v++) {
        alive(packet,v);assert(!wifi_alive_packet(&b,packet,sizeof(packet),v));
        assert(b.alive && b.lmac_major==89 && b.umac_major==89 && b.sku[0]==11 && b.sku[2]==13);
        unsigned total=4+u32(packet);
        for(unsigned n=0;n<total;n++)assert(wifi_alive_packet(&b,packet,n,v));
        assert(wifi_alive_packet(&b,packet,sizeof(packet),v==5?6:5));
    }
    alive(packet,6);packet[8]=0;assert(wifi_alive_packet(&b,packet,sizeof(packet),6));
    alive(packet,6);packet[7]=0;assert(wifi_alive_packet(&b,packet,sizeof(packet),6));
    alive(packet,6);p32(packet,0x55550000);assert(wifi_alive_packet(&b,packet,sizeof(packet),6));
    alive(packet,6);p32(packet,4093);assert(wifi_alive_packet(&b,packet,sizeof(packet),6));
    alive(packet,6);packet[4]=2;b.alive=0;assert(!wifi_alive_packet(&b,packet,sizeof(packet),6)&&!b.alive);
}
enum {OK,TIMEOUT,CLOCK,LOCK,READY,RFKILL,FWERR,DISAPPEAR,BAD_LENGTH,BAD_STATUS,FRAGMENT,BAD_PRODUCER,
      EARLY_RX,STOP_TIMEOUT,DISABLE_FAIL,ENABLE_FAIL,MALFORMED_THEN_ALIVE,NOTIFICATION_FLOOD,INITIAL_DISABLE_FAIL,PNVM_OK,PNVM_TIMEOUT,PNVM_STATUS,PNVM_SHORT,PNVM_EARLY,PNVM_LOCK,PNVM_STOP,PNVM_ALLOC,PNVM_SKU,NVM_OK,NVM_OPAQUE_STATUS,NVM_TIMEOUT,NVM_INIT_TIMEOUT,NVM_BAD_TAG,NVM_SHORT,NVM_BAD_COUNT,NVM_CMD_FAILED,NVM_FW_ERROR,NVM_ALLOC,NVM_INIT_FIRST};
typedef struct {
    uint32_t reg[4096];
    WifiBoot *b;
    unsigned fault,started,master,ever_master,writes,us,disable_calls,initial_rx;
} Device;
static uint32_t read_reg(void *opaque,unsigned r) {
    Device *d=opaque;assert(r%4==0 && r<16384);
    uint32_t v=d->reg[r/4];
    if(r==0x24) {
        if(d->fault!=RFKILL)v|=0x08000000;
        if(d->fault==CLOCK)v&=~1U;
        else if(v&4)v|=1;
        if((d->fault==LOCK || (d->fault==PNVM_LOCK && d->b->alive)) && (v&8))v|=0x10;
    }
    if(r==0 && d->fault==READY)v&=~0x400000U;
    if(r==0x20 && (v&0x200) && d->fault!=STOP_TIMEOUT && d->fault!=PNVM_STOP)v|=0x100;
    if(r==8 && d->started && d->fault==DISAPPEAR)return UINT32_MAX;
    return v;
}
static void push_packet(Device *d) {
    if(d->fault==TIMEOUT || d->fault==FWERR || d->fault==DISAPPEAR)return;
    alive(d->b->rx,image.alive_version);
    if(image.pnvm.data) {
        p32(d->b->rx+124,0x510d1);p32(d->b->rx+128,0);p32(d->b->rx+132,d->fault==PNVM_SKU?1:0);
    }
    if(d->fault==PNVM_EARLY){p32(d->b->rx,8);d->b->rx[4]=0xfe;d->b->rx[5]=12;p32(d->b->rx+8,0);}
    if(d->fault==BAD_LENGTH)p32(d->b->rx,4093);
    if(d->fault==BAD_STATUS)d->b->rx[8]=0;
    if(d->fault==FRAGMENT)d->b->used_ring[6]=1;
    if(d->fault==MALFORMED_THEN_ALIVE) {
        /* Valid unrelated notification followed by ALIVE. */
        d->b->rx[4]=0xf1;alive(d->b->rx+4096,image.alive_version);
    }
    unsigned count=d->fault==MALFORMED_THEN_ALIVE?2:1;
    if(d->fault==BAD_PRODUCER)count=512;
    if(d->fault==NOTIFICATION_FLOOD) {
        count=505;
        for(unsigned n=0;n<count;n++){alive(d->b->rx+n*4096,6);d->b->rx[n*4096+4]=0xf1;}
    }
    *(uint16_t *)(void *)d->b->status=count;
}
static void write_reg(void *opaque,unsigned r,uint32_t v) {
    Device *d=opaque;assert(r%4==0 && r<16384);++d->writes;
    if(r==8 || r==0x10){d->reg[r/4]&=~v;return;}
    if(r==0x20 && (v&0x80)){d->reg[r/4]=0;d->reg[0x24/4]&=~5U;return;}
    if(r==0x1c80) {
        assert(d->started);
        if(!d->initial_rx){
            assert(v==8);
            for(unsigned n=0;n<512;n++) {
                assert(u32(d->b->free_ring+n*16)==n);
                assert(u64(d->b->free_ring+n*16+8)==(uintptr_t)d->b->rx+n*4096);
            }
            d->initial_rx=1;push_packet(d);
        }
    }
    if(r==0x44c && d->reg[0x444/4]==0x3d05c44 && v==1) {
        assert(d->master && u64(d->b->context+88)==(uintptr_t)d->b->scratch);
        assert(((uint64_t)d->reg[0x11c/4]<<32|d->reg[0x118/4])==(uintptr_t)d->b->context);
        assert(((uint64_t)d->reg[0x124/4]<<32|d->reg[0x120/4])==(uintptr_t)d->b->iml);
        assert(d->reg[0x128/4]==image.iml.size);
        d->started=1;d->reg[8/4]=1;
        /* ROM may overwrite free descriptors while configuring RFH. */
        memset(d->b->free_ring,0,512*16);
        if(d->fault==FWERR)d->reg[8/4]|=1U<<25;
        if(d->fault==EARLY_RX){d->reg[8/4]=0;push_packet(d);}
    }
    if(r==0x44c && d->reg[0x444/4]==0x3d05c04 && v==(1U<<20)) {
        assert(d->master && d->b->pnvm_started && (d->reg[0x24/4]&8));
        assert(u64(d->b->scratch+16) && u32(d->b->scratch+24)==13708);
        if(d->fault!=PNVM_TIMEOUT) {
            uint8_t *packet=d->b->rx+d->b->consumed*4096;
            memset(packet,0,4096);p32(packet,d->fault==PNVM_SHORT?4:8);
            packet[4]=0xfe;packet[5]=12;packet[7]=0x80;p32(packet+8,(d->fault==PNVM_STATUS || d->fault==NVM_OPAQUE_STATUS)?0x50001:0);
            *(uint16_t *)(void *)d->b->status=d->b->consumed+1;
        }
    }
    if(r==0x460) {
        assert(d->master && d->b->pnvm_complete && (d->reg[0x24/4]&8));
        assert(v>=1 && v<=3);unsigned index=v-1;
        uint8_t *cmd=d->b->command_data+index*64,*desc=d->b->tx+index*256;
        assert((u32(desc)&65535)==1 && (u32(desc)>>16)==12 && u64(desc+4)==(uintptr_t)cmd);
        assert(cmd[0]==(index==0?3:index==1?0:2) && cmd[1]==(index==0?2:12));
        assert(cmd[2]==index && cmd[3]==0 && u32(cmd+4)==4 && u32(cmd+8)==(index==0?2:0));
        if(d->fault!=NVM_TIMEOUT) {
            unsigned n=d->b->consumed;uint8_t *packet=d->b->rx+n*4096;
            memset(packet,0,4096);packet[4]=cmd[0];packet[5]=cmd[1];packet[6]=index;
            p32(packet,index==2?472:4);
            if(index==2) {
                p32(packet+8,0);packet[12]=0x34;packet[13]=0x12;packet[15]=4;
                p32(packet+16,0x1f);p32(packet+20,3);p32(packet+24,3);
                p32(packet+28,1);p32(packet+32,110);
                for(unsigned c=0;c<110;c++)p32(packet+36+c*4,1);
            }
            if(d->fault==NVM_BAD_TAG)packet[6]=15;
            if(d->fault==NVM_SHORT && index==2)p32(packet,8);
            if(d->fault==NVM_BAD_COUNT && index==2)p32(packet+32,111);
            if(d->fault==NVM_CMD_FAILED)packet[5]|=0x40;
            if(d->fault==NVM_FW_ERROR){packet[4]=2;packet[5]=0;p32(packet,24);}
            ++n;
            if(index==1 && d->fault!=NVM_INIT_TIMEOUT) {
                uint8_t *init=d->b->rx+n*4096;memset(init,0,4096);p32(init,4);init[4]=4;init[7]=0x80;
                if(d->fault==NVM_INIT_FIRST) {
                    uint8_t temp[4096];memcpy(temp,init,4096);memcpy(init,packet,4096);memcpy(packet,temp,4096);
                }
                ++n;
            }
            *(uint16_t *)(void *)d->b->status=n;
        }
    }
    d->reg[r/4]=v;
}
static void write_byte(void *opaque,unsigned r,uint8_t v) {
    Device *d=opaque;assert(r==4||r==5);((uint8_t *)d->reg)[r]=v;++d->writes;
}
static void delay(void *opaque,unsigned us){Device *d=opaque;d->us+=us;assert(d->us<10000000);}
static int mastering(void *opaque,int enable) {
    Device *d=opaque;
    if(!enable) {
        ++d->disable_calls;
        if(d->fault==INITIAL_DISABLE_FAIL || (d->fault==DISABLE_FAIL && d->disable_calls>1))return 0;
    }
    if(enable && d->fault==ENABLE_FAIL)return 0;
    d->master=enable;d->ever_master|=enable;return 1;
}
static void log_value(void *opaque,const char *name,uint64_t value){
    (void)value;Device *d=opaque;
    if(d->fault==NVM_ALLOC && !strcmp(name,"AX211_PNVM_COMPLETE"))d->b->capacity=d->b->used;
    if(d->fault==PNVM_ALLOC && !strcmp(name,"AX211_FW_ALIVE"))d->b->capacity=d->b->used;
}
static void transport_test(void) {
    for(unsigned fault=OK;fault<=INITIAL_DISABLE_FAIL;fault++) {
        WifiBoot b={.pool=pool,.capacity=sizeof(pool)};
        Device d={.b=&b,.fault=fault};
        WifiOps ops={&d,read_reg,write_reg,write_byte,delay,mastering,log_value};
        const char *error=wifi_firmware_start(&b,&image,&ops,0x441);
        int succeeds=fault==OK || fault==MALFORMED_THEN_ALIVE;
        assert((error==0)==succeeds);
        if(fault==STOP_TIMEOUT || fault==DISABLE_FAIL || fault==INITIAL_DISABLE_FAIL)assert(!b.quiesced);
        else assert(b.quiesced && !d.master);
        if(fault==CLOCK || fault==LOCK || fault==READY || fault==RFKILL || fault==INITIAL_DISABLE_FAIL)assert(!d.ever_master);
        else if(fault!=ENABLE_FAIL)assert(d.ever_master);
        unsigned writes=d.writes;
        assert(wifi_firmware_start(&b,&image,&ops,0x441));assert(writes==d.writes);
    }
}
static void parser_test(void) {
    WifiImage invalid;
    /* Every TLV boundary/header/payload truncation, including the final pad. */
    for(size_t at=88;at<blob_size;) {
        uint32_t n=u32(blob+at+4);
        assert(wifi_image_parse(blob,at+1,&invalid));
        assert(wifi_image_parse(blob,at+7,&invalid));
        if(n)assert(wifi_image_parse(blob,at+8+n-1,&invalid));
        at+=8+((n+3)&~3U);
    }
    assert(wifi_image_parse(blob,87,&invalid));
    uint8_t save=blob[4];blob[4]=0;assert(wifi_image_parse(blob,blob_size,&invalid));blob[4]=save;
    uint32_t n=u32(blob+92);p32(blob+92,UINT32_MAX);assert(wifi_image_parse(blob,blob_size,&invalid));p32(blob+92,n);
    for(size_t at=88;at<blob_size;) {
        uint32_t type=u32(blob+at),len=u32(blob+at+4);
        if(type==19 || type==30 || type==36 || type==48 || type==52) {
            uint32_t saved=u32(blob+at+4);
            p32(blob+at+4,type==19?3:type==36?4:type==48?3:0);
            assert(wifi_image_parse(blob,blob_size,&invalid));p32(blob+at+4,saved);
        }
        if(type==36) {
            uint32_t saved=u32(blob+at+8);p32(blob+at+8,90);
            assert(wifi_image_parse(blob,blob_size,&invalid));p32(blob+at+8,saved);
        }
        if(type==19 && u32(blob+at+8)==0xffffcccc) {
            p32(blob+at+8,0xaaaabbbb);assert(wifi_image_parse(blob,blob_size,&invalid));
            p32(blob+at+8,0xffffcccc);
        }
        at+=8+((len+3)&~3U);
    }
    assert(!wifi_image_parse(blob,blob_size,&image));
    assert(image.count[0]==15 && image.count[1]==17 && image.count[2]==26 && image.iml.size==13944);
    assert(image.alive_version==6 && image.version[0]==89);
}
int main(int argc,char **argv) {
    assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);
    assert(!fseek(f,0,SEEK_END));long size=ftell(f);assert(size>0 && size<16*1024*1024);rewind(f);
    blob_size=size;blob=malloc(size);assert(blob && fread(blob,1,size,f)==(size_t)size);fclose(f);
    parser_test();layout_test();packet_test();transport_test();free(blob);
    puts("AX211_STARTUP_HOST_OK parser/layout/ALIVE/19 transport scenarios; hardware untested");
    return 0;
}
