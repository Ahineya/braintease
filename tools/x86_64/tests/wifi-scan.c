/* Production radio/scan path with native DMA/CSR mock. No RF success claim. */
#define main wifi_start_tests
#include "wifi-start.c"
#undef main
enum {SCAN_OK=100,SCAN_TIMEOUT,SCAN_BAD_TAG,SCAN_BAD_MCC,SCAN_NO_CHANNEL,
      SCAN_ABORT,SCAN_BAD_UID,SCAN_BAD_MAC,SCAN_OTP,SCAN_EARLY_COMPLETE,
      SCAN_ALLOC,SCAN_THERMAL,SCAN_BEFORE_ACK,SCAN_CMD_FAILED,SCAN_COMPLETION_TIMEOUT,
      SCAN_EMPTY,SCAN_REG_CHANGE,SCAN_RX_BOUNDS,SCAN_MCC_REJECT,SCAN_5_ONLY,
      SCAN_5_NVM_DENIED,SCAN_5_MCC_DENIED,SCAN_5_UNREQUESTED,SCAN_SHORT_PROFILE,
      SCAN_REG_SHORT,SCAN_REG_BAD_COUNTRY,SCAN_REG_FILTER,SCAN_REG_RACE,
      SCAN_REG_STORM,SCAN_REG_REPLY_SHORT,SCAN_REG_TIMEOUT,SCAN_REG_FLOOD,SCAN_REG_WORLD,
      SCAN_REG_SOURCE,SCAN_REG_COUNTRY_REPLY,SCAN_REG_RESERVED,SCAN_REG_EARLY,
      SCAN_REG_EXPAND,SCAN_REG_EXPAND_STALE,SCAN_TARGET_LATE,SCAN_TARGET_ABSENT};
static unsigned scan_fault;
static int regulatory_case(void){return scan_fault==SCAN_REG_CHANGE ||
    (scan_fault>=SCAN_REG_SHORT && scan_fault<=SCAN_REG_EXPAND_STALE);}
static int expand_case(void){return scan_fault==SCAN_REG_EXPAND || scan_fault==SCAN_REG_EXPAND_STALE;}
static void country_notice(uint8_t *p) {
    memset(p,0,4096);p32(p,8);p[4]=0xc9;p[6]=7;p[7]=0xc0;p32(p+8,0x0005414c);
}
static uint32_t scan_read(void *opaque,unsigned reg) {
    if(reg==0x388)return scan_fault==SCAN_BAD_MAC || scan_fault==SCAN_OTP?0:0x00112233;
    if(reg==0x38c)return scan_fault==SCAN_BAD_MAC || scan_fault==SCAN_OTP?0:0x4455;
    if(reg==0x380)return scan_fault==SCAN_OTP?0x00665544:0;
    if(reg==0x384)return scan_fault==SCAN_OTP?0x3322:0;
    return read_reg(opaque,reg);
}
static void beacon(uint8_t *packet) {
    memset(packet,0,4096);uint8_t *p=packet+8,*f=p+64;
    p32(packet,4+64+43);packet[4]=0xc1;packet[7]=0x80;
    p[0]=43;p32(p+12,3);p[40]=55;p[41]=60;p[42]=6;
    f[0]=0x80;for(unsigned n=0;n<6;n++)f[16+n]=n*2;
    f[34]=0x10;f[36]=0;f[37]=3;f[38]='a';f[39]='b';f[40]='c';f[41]=48;f[42]=0;
}
static void scan_write(void *opaque,unsigned reg,uint32_t v) {
    Device *d=opaque;
    if(reg!=0x460 || v<=3){write_reg(opaque,reg,v);return;}
    assert(d->master && d->b->nvm_complete && (d->reg[0x24/4]&8));
    uint8_t *cmd=d->b->command_data,*desc=d->b->tx+(v-1)*256;
    unsigned scan=cmd[0]==13 && cmd[1]==1,refresh=v>12 && !scan,step=scan?8:refresh?5:v-4;
    assert(step<9 && v<=14);
    const unsigned codes[9]={0x98,0x9b,1,4,0x77,0xc8,12,0xd2,13};
    const unsigned groups[9]={1,1,2,4,1,1,1,1,1};
    const unsigned sizes[9]={4,8,8,20,4,28,12,60,1940};
    unsigned total=sizes[step]+8;
    assert((uintptr_t)cmd%64==0 && cmd[0]==codes[step] && cmd[1]==groups[step]);
    assert(cmd[2]==v-1 && cmd[3]==0 && u32(cmd+4)==sizes[step]);
    assert((u32(desc)&65535)==(total>20?2:1) && u32(desc)>>16==(total>20?20:total));
    assert(u64(desc+4)==(uintptr_t)cmd);
    if(total>20)assert((u32(desc+12)&65535)==total-20 && u64(desc+14)==(uintptr_t)cmd+20);
    if(step==5) {
        if(refresh)assert(cmd[8]=='L' && cmd[9]=='A' && cmd[10]==5); /* LE AL, WIFI */
        else assert(cmd[8]=='Z' && cmd[9]=='Z' && cmd[10]==0x10);
    }
    if(step==8) {
        const uint8_t *p=cmd+8;
        assert(u32(p)==(expand_case() && v>12?256U:(v-12)*256) && u32(p+4)==6 && (p[8]|p[9]<<8)==0x886);
        assert(p[45]==(scan_fault==SCAN_SHORT_PROFILE?3:scan_fault==SCAN_5_ONLY?4:
            scan_fault==SCAN_5_NVM_DENIED || scan_fault==SCAN_5_MCC_DENIED || (expand_case() && v==12)?6:7));
        if(scan_fault!=SCAN_5_ONLY)assert( p[50]==0 && p[52]==1 && p[60]==6 && p[68]==11);
        if(scan_fault!=SCAN_5_ONLY)assert(u32(p+48)==1U<<30 && p[53]==128 && p[54]==1);
        if(scan_fault!=SCAN_SHORT_PROFILE) {
            unsigned start=scan_fault==SCAN_5_ONLY?0:3;
            assert(u32(p+48+start*8)==0 && p[52+start*8]==36);
            assert(p[53+start*8]==128 && p[54+start*8]==1);
            if(scan_fault!=SCAN_5_NVM_DENIED && scan_fault!=SCAN_5_MCC_DENIED && !(expand_case() && v==12))
                assert(p[52+(start+3)*8]==48);
        }
        assert(p[586]==1 && (p[598]|p[599]<<8)==26);
        assert(p[604]==42 && p[606]==10 && p[612]==52 && p[658]==1 && p[660]==0x8c);
    }
    if(scan_fault==SCAN_TIMEOUT || (refresh && scan_fault==SCAN_REG_TIMEOUT))return;
    unsigned n=d->b->consumed;uint8_t *packet=d->b->rx+n*4096;
    memset(packet,0,4096);p32(packet,4);packet[4]=cmd[0];packet[5]=cmd[1];packet[6]=v-1;
    if(step==5) {
        p32(packet,4+20+110*4);p32(packet+8,0);packet[12]=refresh?'L':'0';packet[13]=refresh?'A':'0';packet[20]=refresh?5:0x10;p32(packet+24,110);
        for(unsigned c=0;c<110;c++)p32(packet+28+c*4,scan_fault==SCAN_NO_CHANNEL?0:1);
        if(scan_fault==SCAN_BAD_MCC)p32(packet+24,111);
        if(scan_fault==SCAN_MCC_REJECT)p32(packet+8,4);
        if(refresh && scan_fault==SCAN_REG_REPLY_SHORT)p32(packet,20);
        if(refresh && scan_fault==SCAN_REG_WORLD){packet[12]=packet[13]=0;packet[20]=7;}
        if(refresh && scan_fault==SCAN_REG_COUNTRY_REPLY)packet[12]='l';
        if(refresh && scan_fault==SCAN_REG_RESERVED){packet[21]=3;packet[22]=0xb6;packet[23]=0xa5;}
        if(refresh && scan_fault==SCAN_REG_FILTER)p32(packet+28+17*4,0);
        if(!refresh && expand_case())p32(packet+28+17*4,0);
        if(scan_fault==SCAN_5_MCC_DENIED)p32(packet+28+17*4,0);
        if(scan_fault==SCAN_SHORT_PROFILE){p32(packet,4+20+11*4);p32(packet+24,11);}
    }
    if(scan_fault==SCAN_BAD_TAG)packet[6]=99;
    if(scan_fault==SCAN_CMD_FAILED)packet[5]|=0x40;
    ++n;
    if(scan_fault==SCAN_THERMAL) {
        packet=d->b->rx+n++*4096;memset(packet,0,4096);p32(packet,8);packet[4]=0xfe;packet[5]=4;packet[7]=0x80;
    }
    if(step==4 && scan_fault==SCAN_REG_EARLY) {
        packet=d->b->rx+n++*4096;country_notice(packet);
    }
    if((step==8 && scan_fault!=SCAN_COMPLETION_TIMEOUT) || scan_fault==SCAN_EARLY_COMPLETE) {
        uint8_t *ack=d->b->rx+(n-1)*4096;
        if(scan_fault!=SCAN_EMPTY && scan_fault!=SCAN_TARGET_ABSENT && !(scan_fault==SCAN_TARGET_LATE && v==12)) {
            if(scan_fault!=SCAN_5_ONLY) {
                packet=d->b->rx+n++*4096;beacon(packet);
            }
            packet=d->b->rx+n++*4096;beacon(packet);packet[8+42]=scan_fault==SCAN_5_UNREQUESTED?165:48;
            if(scan_fault==SCAN_RX_BOUNDS)p32(packet+8,4095);
        }
        if(regulatory_case() && scan_fault!=SCAN_REG_EARLY && !(expand_case() && v>12)) {
            unsigned notices=scan_fault==SCAN_REG_FLOOD?9:1;
            for(unsigned j=0;j<notices;j++) {
                packet=d->b->rx+n++*4096;country_notice(packet);
                if(scan_fault==SCAN_REG_SHORT)p32(packet,7);
                if(scan_fault==SCAN_REG_SOURCE)packet[10]=6;
                if(scan_fault==SCAN_REG_BAD_COUNTRY)p32(packet+8,0x0005616c);
            }
        }
        if(scan_fault==SCAN_REG_EXPAND_STALE && v>12) {
            packet=d->b->rx+n++*4096;memset(packet,0,4096);p32(packet,20);packet[4]=15;packet[7]=0x80;packet[14]=1;
        }
        packet=d->b->rx+n++*4096;memset(packet,0,4096);p32(packet,20);packet[4]=15;packet[7]=0x80;
        if(step==8)p32(packet+8,u32(cmd+8));
        packet[14]=scan_fault==SCAN_ABORT?2:1;
        if(scan_fault==SCAN_BAD_UID)p32(packet+8,99);
        if(scan_fault==SCAN_BEFORE_ACK) {
            uint8_t tmp[4096];memcpy(tmp,ack,4096);memcpy(ack,packet,4096);memcpy(packet,tmp,4096);
        }
    }
    if(refresh && ((scan_fault==SCAN_REG_RACE && v==13) || scan_fault==SCAN_REG_STORM)) {
        packet=d->b->rx+n++*4096;country_notice(packet);
    }
    *(uint16_t *)(void *)d->b->status=n;
    d->reg[reg/4]=v;
}
static void scan_log(void *opaque,const char *name,uint64_t v) {
    Device *d=opaque;log_value(opaque,name,v);
    if(!strcmp(name,"AX211_NVM_MAC_FLAGS")) {
        if(scan_fault==SCAN_5_ONLY)d->b->nvm_mac_flags=2;
        if(scan_fault==SCAN_5_NVM_DENIED)d->b->nvm_channel_flags[17]=0;
    }
    if(scan_fault==SCAN_ALLOC && !strcmp(name,"AX211_NVM_MAC_FLAGS"))d->b->capacity=d->b->used;
}
static void parser_checks(void) {
    /* Photographed CHUB notice: country AL, source WIFI, firmware-origin qid. */
    uint8_t country[8]={0xc9,0,7,0xc0,0x4c,0x41,5,0};
    WifiBoot detected={.regulatory_done=1,.scan_started=1};
    assert(!wifi_scan_notification(&detected,country,sizeof(country)));
    assert(detected.reg_country==0x414c && detected.reg_source==5 && detected.reg_generation==1);
    assert(detected.nvm_progress==0); /* a notice cannot postpone a deadline */
    for(unsigned n=0;n<8;n++)assert(wifi_scan_notification(&detected,country,n));
    /* Exact20-byte MCC header photographed on the HP (API6): SAME profile,
     * world00, source DEFAULT7, nonzero byte13=3,110 channels. The photograph
     * only shows the first three channel entries; the rest are synthetic. */
    uint8_t reply[20+110*4]={1,0,0,0,0x30,0x30,0x7e,0x21,0,0x10,0,0,7,3,0,0,110,0,0,0};
    for(unsigned n=0;n<110;n++)p32(reply+20+n*4,n<3?0x34b:1);
    WifiBoot photographed={.command_code=0xc8,.nvm_mac_flags=3,.nvm_channels=110};
    for(unsigned n=0;n<18;n++)photographed.nvm_channel_flags[n]=1;
    assert(!wifi_scan_reply(&photographed,reply,sizeof(reply)));
    assert(photographed.reg_applied_country==0x3030 && photographed.reg_applied_source==7 &&
        photographed.reg_profile_status==1 && photographed.scan_channels_5==15 && photographed.reg_profile_aux==3);
    for(unsigned n=0;n<sizeof(reply);n++)assert(wifi_scan_reply(&photographed,reply,n));
    reply[12]=6;assert(wifi_scan_reply(&photographed,reply,sizeof(reply)));reply[12]=7;
    reply[4]='l';assert(wifi_scan_reply(&photographed,reply,sizeof(reply)));reply[4]='0';
    uint8_t p[4096];beacon(p);WifiBoot b={.scan_channels=1U<<6};
    assert(!wifi_scan_mpdu(&b,p+8,107) && b.scan_count==1 && b.aps[0].energy==55 && b.aps[0].rsn);
    assert(!wifi_scan_mpdu(&b,p+8,107) && b.scan_count==1);
    for(unsigned n=0;n<107;n++)assert(wifi_scan_mpdu(&b,p+8,n));
    p[8+64+37]=255;assert(!wifi_scan_mpdu(&b,p+8,107) && b.scan_bad_frames==1);
    beacon(p);p[8+12]=0;b.scan_count=0;assert(!wifi_scan_mpdu(&b,p+8,107) && !b.scan_count);
    beacon(p);p[8+64+16]|=1;assert(!wifi_scan_mpdu(&b,p+8,107) && !b.scan_count);
    beacon(p);p[8+64+36]=1;assert(!wifi_scan_mpdu(&b,p+8,107) && !b.scan_count);
    /* More than32 BSSIDs remains a bounded table, duplicates do not grow it. */
    b=(WifiBoot){.scan_channels=1U<<6};
    for(unsigned n=0;n<40;n++){beacon(p);p[8+64+21]=n;assert(!wifi_scan_mpdu(&b,p+8,107));}
    assert(b.scan_count==32 && b.scan_dropped==8);
    /* PAD is after the management header, not after the descriptor. */
    beacon(p);uint8_t *d=p+8,*f=d+64;memmove(f+26,f+24,19);f[24]=f[25]=0;
    d[3]=0x20;d[0]=45;b=(WifiBoot){.scan_channels=1U<<6};
    assert(!wifi_scan_mpdu(&b,d,109) && b.scan_count==1 && b.aps[0].ssid_size==3);
    beacon(p);p[8+42]=48;b=(WifiBoot){.scan_channels_5=8};
    assert(!wifi_scan_mpdu(&b,p+8,107) && b.scan_count==1 && b.aps[0].channel==48);
    p[8+42]=44;assert(!wifi_scan_mpdu(&b,p+8,107) && b.scan_count==1);
    p[8+42]=255;assert(!wifi_scan_mpdu(&b,p+8,107) && b.scan_count==1);
    uint8_t reserved[6]={2,0xcc,0xaa,0xff,0xee,0};assert(!wifi_mac_valid(reserved));
}
#ifndef WIFI_SCAN_MAIN
#define WIFI_SCAN_MAIN main
#endif
#include "wifi-scan-recovery.inc"
int WIFI_SCAN_MAIN(int argc,char **argv) {
    assert(argc==3);char *args[]={argv[0],argv[1],0};assert(!wifi_start_tests(2,args));
    FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
    blob=malloc(size);assert(blob && fread(blob,1,size,f)==(size_t)size);fclose(f);
    assert(!wifi_image_parse(blob,size,&image));
    f=fopen(argv[2],"rb");assert(f);fseek(f,0,SEEK_END);size=ftell(f);rewind(f);
    uint8_t *pnvm=malloc(size);assert(pnvm && fread(pnvm,1,size,f)==(size_t)size);fclose(f);
    image.pnvm=(WifiSection){pnvm,(unsigned)size};image.nvm_probe=image.scan_probe=1;
    assert(!wifi_scan_validate(&image));parser_checks();recovery_checks();
    for(unsigned which=0;which<5;which++) {
        WifiImage bad=image;
        if(which==0)bad.scan_request_version=18;
        if(which==1)bad.rx_notification=6;
        if(which==2)bad.mcc_notification=8;
        if(which==3)bad.api[1]&=~(1U<<24);
        if(which==4)bad.capabilities[0]|=1U<<12;
        WifiBoot b={.pool=pool,.capacity=sizeof(pool)};Device d={.b=&b};
        WifiOps ops={&d,scan_read,scan_write,write_byte,delay,mastering,scan_log};
        assert(wifi_firmware_start(&b,&bad,&ops,0x441) && !d.writes && !d.ever_master && !b.attempted);
    }
    for(scan_fault=SCAN_OK;scan_fault<=SCAN_REG_EXPAND_STALE;scan_fault++) {
        WifiBoot b={.pool=pool,.capacity=sizeof(pool)};Device d={.b=&b,.fault=NVM_OPAQUE_STATUS};
        WifiOps ops={&d,scan_read,scan_write,write_byte,delay,mastering,scan_log};
        const char *error=wifi_firmware_start(&b,&image,&ops,0x441);
        int success=scan_fault==SCAN_MCC_REJECT || scan_fault==SCAN_OK || scan_fault==SCAN_OTP || scan_fault==SCAN_BEFORE_ACK || scan_fault==SCAN_EMPTY ||
            (scan_fault>=SCAN_5_ONLY && scan_fault<=SCAN_SHORT_PROFILE) || scan_fault==SCAN_REG_CHANGE ||
            scan_fault==SCAN_REG_FILTER || scan_fault==SCAN_REG_RACE || scan_fault==SCAN_REG_WORLD || scan_fault==SCAN_REG_EARLY || scan_fault==SCAN_REG_RESERVED || expand_case();
        if((error==0)!=success)fprintf(stderr,"fault %u: %s\n",scan_fault,error?error:"unexpected success");
        assert((error==0)==success && b.quiesced && !d.master);
        if(success) {
            unsigned count=scan_fault==SCAN_EMPTY?0:
                (scan_fault>=SCAN_5_ONLY && scan_fault<=SCAN_SHORT_PROFILE) || scan_fault==SCAN_REG_FILTER?1:2;
            unsigned commands=scan_fault==SCAN_REG_RACE || expand_case()?14:regulatory_case()?13:12;
            assert(b.scan_complete && b.scan_acked && b.scan_count==count && b.command_count==commands);
            assert(b.reg_generation==b.reg_applied_generation);
            if(scan_fault==SCAN_REG_RESERVED)assert(b.reg_profile_aux==0xa5b603 && b.reg_applied_source==5);
            if(scan_fault==SCAN_REG_WORLD)assert(b.reg_country==0x414c && b.reg_applied_country==0x3030 && b.reg_applied_source==7);
            if(scan_fault==SCAN_REG_FILTER)assert(b.reg_filtered==1 && b.aps[0].channel==6);
            if(scan_fault==SCAN_REG_CHANGE)assert(b.reg_country==0x414c && b.reg_refreshes==1 &&
                b.reg_applied_country==0x414c && b.reg_applied_source==5);
            if(scan_fault==SCAN_5_ONLY)assert(b.scan_channels==0 && b.scan_channels_5==15 && b.aps[0].channel==48);
            if(scan_fault==SCAN_5_NVM_DENIED || scan_fault==SCAN_5_MCC_DENIED)assert(b.scan_channels_5==7);
            if(scan_fault==SCAN_SHORT_PROFILE)assert(b.scan_channels_5==0);
            if(expand_case())assert(b.scan_attempts==2 && b.scan_uid==256 && b.scan_channels_5==15 && b.reg_refreshes==1);
        }
    }
    free(blob);free(pnvm);
    puts("AX211_SCAN_HOST_OK passive v17/TFH two-TB/CSR MAC/MCC bounds/beacon guards/39 transport scenarios; no RF acceptance implied");
    return 0;
}
