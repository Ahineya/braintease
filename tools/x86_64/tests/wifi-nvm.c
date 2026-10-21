/* Native command/NVM transport fault injection, including the photographed
 * nonzero PNVM completion status. No Wi-Fi hardware is emulated by QEMU. */
#define main wifi_start_tests
#include "wifi-start.c"
#undef main
static void nvm_parse_test(void) {
    uint8_t p[468]={0};WifiBoot b={0};
    p[4]=1;p[7]=2;p32(p+8,3);p32(p+12,3);p32(p+16,3);p32(p+20,1);p32(p+24,110);
    for(unsigned n=0;n<110;n++)p32(p+28+4*n,1);
    assert(!wifi_nvm_parse(&b,p,sizeof(p),4) && b.nvm_complete && b.nvm_valid_channels==110 && b.nvm_channel_flags[17]==1);
    for(unsigned n=0;n<468;n++)assert(wifi_nvm_parse(&b,p,n,4));
    assert(wifi_nvm_parse(&b,p,468,2));p32(p+24,111);assert(wifi_nvm_parse(&b,p,468,4));p32(p+24,110);
    p32(p+12,4);assert(wifi_nvm_parse(&b,p,468,4));p32(p+12,3);
    p32(p,1);assert(!wifi_nvm_parse(&b,p,468,4) && b.nvm_general_flags==1);p32(p,0);
    memset(p+24,0,104);for(unsigned n=0;n<51;n++){p[24+n*2]=1;}
    assert(!wifi_nvm_parse(&b,p,128,3) && b.nvm_channels==51 && b.nvm_valid_channels==51 && b.nvm_channel_flags[17]==1);
}
int main(int argc,char **argv) {
    assert(argc==3);char *base_argv[]={argv[0],argv[1],0};assert(!wifi_start_tests(2,base_argv));
    FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long bytes=ftell(f);rewind(f);
    blob=malloc(bytes);assert(blob && fread(blob,1,bytes,f)==(size_t)bytes);fclose(f);
    assert(!wifi_image_parse(blob,bytes,&image));assert(image.nvm_notification==4);
    f=fopen(argv[2],"rb");assert(f);fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
    uint8_t *pnvm=malloc(size);assert(pnvm && fread(pnvm,1,size,f)==(size_t)size);fclose(f);
    image.pnvm=(WifiSection){pnvm,(unsigned)size};image.nvm_probe=1;
    nvm_parse_test();
    for(unsigned fault=NVM_OK;fault<=NVM_INIT_FIRST;fault++) {
        WifiBoot b={.pool=pool,.capacity=sizeof(pool)};Device d={.b=&b,.fault=fault};
        WifiOps ops={&d,read_reg,write_reg,write_byte,delay,mastering,log_value};
        const char *error=wifi_firmware_start(&b,&image,&ops,0x441);
        unsigned success=fault==NVM_OK || fault==NVM_OPAQUE_STATUS || fault==NVM_INIT_FIRST;
        assert((error==0)==success && b.quiesced && !d.master);
        if(success)assert(b.nvm_complete && b.init_complete && b.command_count==3 && b.nvm_version==0x1234);
        if(fault==NVM_OPAQUE_STATUS)assert(b.pnvm_status==0x50001);
        if(fault==NVM_TIMEOUT || fault==NVM_INIT_TIMEOUT)assert(strstr(error,"timeout"));
        if(fault==NVM_BAD_TAG)assert(strstr(error,"tag/state"));
        if(fault==NVM_SHORT)assert(strstr(error,"size/version"));
        if(fault==NVM_BAD_COUNT)assert(strstr(error,"channel count"));
        if(fault==NVM_CMD_FAILED)assert(strstr(error,"failed flag"));
        if(fault==NVM_FW_ERROR)assert(strstr(error,"command error"));
        if(fault==NVM_ALLOC)assert(strstr(error,"DMA allocation"));
    }
    free(blob);free(pnvm);
    puts("AX211_NVM_HOST_OK descriptor/header/reply tags/INIT ordering/NVM v3-v4 bounds/11 transport scenarios; physical NVM pending");
    return 0;
}
