/* Share the production startup harness, including firmware parser and DMA/RX
 * mock. Add real PNVM selection/layout and completion/error handshakes. */
#define main wifi_start_tests
#include "wifi-start.c"
#undef main
static uint8_t *pnvm;
static size_t pnvm_bytes;
static const uint32_t hp_sku[3]={0x510d1,0,0};
static void pnvm_parser_test(void) {
    WifiPnvm selected;
    assert(!wifi_pnvm_select(pnvm,pnvm_bytes,hp_sku,0x44,0x10d,&selected));
    assert(selected.count==2 && selected.size==13708 && selected.version==0x38196ea0);
    assert(selected.section[0].size==1656 && selected.section[1].size==12052);
    uint32_t missing[3]={0x510d1,0,1};
    assert(wifi_pnvm_select(pnvm,pnvm_bytes,missing,0x44,0x10d,&selected));
    assert(wifi_pnvm_select(pnvm,pnvm_bytes,hp_sku,0x45,0x10d,&selected));
    assert(wifi_pnvm_select(pnvm,pnvm_bytes,hp_sku,0x44,0x10e,&selected));
    for(size_t at=0;at<pnvm_bytes;) {
        unsigned n=u32(pnvm+at+4),kind=u32(pnvm+at);
        assert(wifi_pnvm_select(pnvm,at+1,hp_sku,0x44,0x10d,&selected));
        assert(wifi_pnvm_select(pnvm,at+7,hp_sku,0x44,0x10d,&selected));
        if(n)assert(wifi_pnvm_select(pnvm,at+8+n-1,hp_sku,0x44,0x10d,&selected));
        p32(pnvm+at+4,kind==64?11:3);
        assert(wifi_pnvm_select(pnvm,pnvm_bytes,hp_sku,0x44,0x10d,&selected));
        p32(pnvm+at+4,n);at+=8+((n+3)&~3U);
    }
    uint8_t *duplicate=malloc(pnvm_bytes+2048);assert(duplicate);memcpy(duplicate,pnvm,pnvm_bytes);
    uint8_t *p=duplicate+pnvm_bytes;p32(p,64);p32(p+4,12);
    for(unsigned n=0;n<3;n++)p32(p+8+4*n,hp_sku[n]);
    assert(wifi_pnvm_select(duplicate,pnvm_bytes+20,hp_sku,0x44,0x10d,&selected));
    for(unsigned n=0;n<64;n++){p32(p+n*16,19);p32(p+n*16+4,8);p32(p+n*16+8,n);p32(p+n*16+12,n);}
    assert(wifi_pnvm_select(duplicate,pnvm_bytes+1024,hp_sku,0x44,0x10d,&selected));free(duplicate);
}
static void pnvm_layout_test(void) {
    WifiPnvm selected;assert(!wifi_pnvm_select(pnvm,pnvm_bytes,hp_sku,0x44,0x10d,&selected));
    for(unsigned fragmented=0;fragmented<2;fragmented++) {
        image.fragmented_pnvm=fragmented;
        WifiBoot b={.pool=pool,.capacity=sizeof(pool)};
        assert(!wifi_layout(&b,&image,0x441));b.alive=1;
        memcpy(b.sku,hp_sku,sizeof(hp_sku));assert(!wifi_pnvm_layout(&b,&image));
        uint8_t *base=(void *)(uintptr_t)u64(b.scratch+16);
        assert(b.pnvm_count==2 && b.pnvm_size==13708 && u32(b.scratch+24)==13708);
        unsigned at=0;
        for(unsigned n=0;n<selected.count;n++) {
            uint8_t *data=fragmented?(void *)(uintptr_t)u64(base+8*n):base+at;
            assert(data>=pool && data+selected.section[n].size<=pool+b.used);
            assert(!memcmp(data,selected.section[n].data,selected.section[n].size));at+=selected.section[n].size;
        }
        if(fragmented)for(unsigned n=2;n<64;n++)assert(!u64(base+n*8));
    }
    image.fragmented_pnvm=1;
}
static void pnvm_transport_test(void) {
    for(unsigned fault=PNVM_OK;fault<=PNVM_SKU;fault++) {
        WifiBoot b={.pool=pool,.capacity=sizeof(pool)};Device d={.b=&b,.fault=fault};
        WifiOps ops={&d,read_reg,write_reg,write_byte,delay,mastering,log_value};
        const char *error=wifi_firmware_start(&b,&image,&ops,0x441);
        assert((error==0)==(fault==PNVM_OK || fault==PNVM_STATUS));assert(!d.master);
        if(fault==PNVM_STOP)assert(!b.quiesced && b.pnvm_complete);
        else assert(b.quiesced);
        if(fault==PNVM_OK || fault==PNVM_STATUS)assert(b.pnvm_complete && b.pnvm_version==0x38196ea0);
        if(fault==PNVM_TIMEOUT)assert(error && strstr(error,"timeout"));
        if(fault==PNVM_STATUS)assert(!error && b.pnvm_complete && b.pnvm_status==0x50001);
        if(fault==PNVM_SHORT || fault==PNVM_EARLY)assert(error && strstr(error,"format/state"));
        if(fault==PNVM_LOCK)assert(error && strstr(error,"lock timeout"));
        if(fault==PNVM_ALLOC)assert(error && strstr(error,"DMA allocation"));
        if(fault==PNVM_SKU)assert(error && strstr(error,"matching SKU") && !b.pnvm_started);
    }
    WifiBoot b={.pool=pool,.capacity=sizeof(pool)};Device d={.b=&b};
    WifiOps ops={&d,read_reg,write_reg,write_byte,delay,mastering,log_value};
    image.pnvm_notification=2;
    assert(wifi_firmware_start(&b,&image,&ops,0x441) && !b.attempted && !d.writes && !d.master);
    image.pnvm_notification=1;
}
int main(int argc,char **argv) {
    assert(argc==3);char *base_argv[]={argv[0],argv[1],0};assert(!wifi_start_tests(2,base_argv));
    FILE *source=fopen(argv[1],"rb");assert(source);assert(!fseek(source,0,SEEK_END));
    long source_size=ftell(source);rewind(source);assert(source_size>0 && source_size<16*1024*1024);
    blob=malloc(source_size);assert(blob && fread(blob,1,source_size,source)==(size_t)source_size);fclose(source);
    assert(!wifi_image_parse(blob,source_size,&image));
    FILE *f=fopen(argv[2],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long size=ftell(f);rewind(f);
    assert(size>0 && size<1024*1024);pnvm_bytes=size;pnvm=malloc(size);assert(pnvm);
    assert(fread(pnvm,1,size,f)==(size_t)size);fclose(f);
    assert(image.fragmented_pnvm==1 && image.pnvm_notification==1);
    image.pnvm=(WifiSection){pnvm,(unsigned)size};
    pnvm_parser_test();pnvm_layout_test();pnvm_transport_test();free(pnvm);free(blob);
    puts("AX211_PNVM_HOST_OK real SKU/2-fragment and contiguous layouts/10 transport/version scenarios; physical PNVM pending");
    return 0;
}
