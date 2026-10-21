/* Production transport exercised end-to-end; byte oracle from net80211 and
 * pinned MLD declarations. Fake MMIO never proves RF/AP acceptance. */
#define WIFI_AUTH_MAIN wifi_auth_tests
#include "wifi-auth.c"
enum {ASSOC_OK,ASSOC_FIRST,ASSOC_PAD,ASSOC_DIRECT,ASSOC_HIGH_SSN,ASSOC_TAIL,
 ASSOC_HEADER,ASSOC_PLAIN_AID,ASSOC_MAX_AID,ASSOC_DTIM,ASSOC_EXTRA_IE,ASSOC_DUP_AP,
 ASSOC_RESERVED_AID,ASSOC_NO_TX,ASSOC_NO_AP,ASSOC_TX_FAIL,ASSOC_TX_SHORT,ASSOC_TX_QUEUE,ASSOC_TX_SSN,
 ASSOC_TX_COUNT,ASSOC_DUP_TX,ASSOC_AP_FAIL,ASSOC_ZERO_AID,ASSOC_BAD_AID,
 ASSOC_WRONG_DEST,ASSOC_WRONG_AP,ASSOC_WRONG_BSSID,ASSOC_CHANNEL,ASSOC_PROTECTED,
 ASSOC_CRC,ASSOC_FIXED_SHORT,ASSOC_IE_SHORT,ASSOC_NO_RATES,ASSOC_UNKNOWN_BASIC,
 ASSOC_CCK_BASIC,ASSOC_NO_BASIC,ASSOC_DEAUTH,ASSOC_SESSION_END,ASSOC_ALLOC,
 ASSOC_COUNTRY,ASSOC_LAST};
static unsigned assoc_fault,update_fault,update_index;
static void assoc_response(uint8_t *p,const WifiBoot *b) {
    memset(p,0,4096);unsigned pad=assoc_fault==ASSOC_PAD?2:0,len=40+pad;
    p[4]=0xc1;p[7]=0x80;uint8_t *d=p+8,*f=d+64,*body=f+24+pad;
    d[3]=pad?0x20:0;p32(d+12,3);d[42]=48;f[0]=0x10;
    memcpy(f+4,b->mac,6);memcpy(f+10,b->target.bssid,6);memcpy(f+16,b->target.bssid,6);
    body[0]=0x11;body[1]=4;body[4]=7;body[5]=0xc0;
    const uint8_t rates[]={1,8,0x8c,18,0x98,36,0xb0,72,96,108};memcpy(body+6,rates,10);
    if(assoc_fault==ASSOC_PLAIN_AID)body[5]=0;
    if(assoc_fault==ASSOC_RESERVED_AID)body[5]=0xf8;
    if(assoc_fault==ASSOC_MAX_AID){body[4]=0xd7;body[5]=0xc7;}
    if(assoc_fault==ASSOC_AP_FAIL)body[2]=17;
    if(assoc_fault==ASSOC_ZERO_AID)body[4]=0;
    if(assoc_fault==ASSOC_BAD_AID){body[4]=0xd8;body[5]=0xc7;}
    if(assoc_fault==ASSOC_WRONG_DEST)f[4]^=2;
    if(assoc_fault==ASSOC_WRONG_AP)f[10]^=2;
    if(assoc_fault==ASSOC_WRONG_BSSID)f[16]^=2;
    if(assoc_fault==ASSOC_CHANNEL)d[42]=44;
    if(assoc_fault==ASSOC_PROTECTED)f[1]=0x40;
    if(assoc_fault==ASSOC_CRC)p32(d+12,0);
    if(assoc_fault==ASSOC_FIXED_SHORT)len=29;
    if(assoc_fault==ASSOC_IE_SHORT)body[7]=9;
    if(assoc_fault==ASSOC_NO_RATES)body[6]=99;
    if(assoc_fault==ASSOC_UNKNOWN_BASIC)body[8]=0xff;
    if(assoc_fault==ASSOC_CCK_BASIC)body[8]=0x82;
    if(assoc_fault==ASSOC_NO_BASIC){body[8]&=127;body[10]&=127;body[12]&=127;}
    if(assoc_fault==ASSOC_DEAUTH)f[0]=0xc0;
    if(assoc_fault==ASSOC_EXTRA_IE){body[16]=221;body[17]=3;memset(body+18,0x5a,3);len+=5;}
    d[0]=len;p32(p,4+64+len);
}
static void assoc_completion(uint8_t *p) {
    tx_completion(p);p32(p+8+44,2);
    if(assoc_fault==ASSOC_DIRECT)p[8+40]=2;
    if(assoc_fault==ASSOC_HIGH_SSN)p32(p+8+44,0xabcd0002);
    if(assoc_fault==ASSOC_TAIL)p32(p,56);
    if(assoc_fault==ASSOC_HEADER){p[6]=55;p[7]=0x81;}
    if(assoc_fault==ASSOC_TX_FAIL)p[8+40]=0x82;
    if(assoc_fault==ASSOC_TX_SHORT)p32(p,51);
    if(assoc_fault==ASSOC_TX_QUEUE)p[8+36]=2;
    if(assoc_fault==ASSOC_TX_SSN)p32(p+8+44,0x1002);
    if(assoc_fault==ASSOC_TX_COUNT)p[8]=2;
}
static void assoc_write(void *opaque,unsigned reg,uint32_t v) {
    Device *d=opaque;WifiBoot *b=d->b;
    if(reg==0x460 && v==0x10002) {
        assert(d->master && b->assoc_sent && b->auth_tx_done && b->auth_received);
        uint8_t *desc=b->management_tx+256,*cmd=(void *)(uintptr_t)u64(desc+4),*f=cmd+32;
        assert(u32(cmd)==0x0101001c && u32(cmd+4)==0x00030041 && u32(cmd+8)==0xc00 && u32(cmd+20)==0x4100);
        assert((u32(desc)&65535)==3 && u32(desc)>>16==20 && (u32(desc+12)&65535)==36);
        assert(u64(desc+14)==(uintptr_t)cmd+20 && (u32(desc+22)&65535)==41 && u64(desc+24)==(uintptr_t)cmd+64);
        assert(!f[0] && !f[1] && f[22]==16 && !f[23]);
        assert(!memcmp(f+4,b->target.bssid,6) && !memcmp(f+10,b->mac,6) && !memcmp(f+16,b->target.bssid,6));
        uint8_t expected[]={0x11,4,1,0,0,3,'a','b','c',1,8,0x8c,18,0x98,36,0xb0,72,96,108,
            48,20,1,0,0,15,172,2,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
        expected[26]=beacon_group;assert(!memcmp(cmd+64,expected,sizeof(expected)));
        assert((u32(b->management_bc)&65535)==30 && u32(b->management_bc)>>16==65);
        unsigned n=b->consumed;
        if(assoc_fault==ASSOC_FIRST)assoc_response(b->rx+n++*4096,b);
        if(assoc_fault!=ASSOC_NO_TX)assoc_completion(b->rx+n++*4096);
        if(assoc_fault!=ASSOC_NO_AP && assoc_fault!=ASSOC_FIRST)assoc_response(b->rx+n++*4096,b);
        if(assoc_fault==ASSOC_DUP_TX)assoc_completion(b->rx+n++*4096);
        if(assoc_fault==ASSOC_DUP_AP)assoc_response(b->rx+n++*4096,b);
        if(assoc_fault==ASSOC_COUNTRY)country_notice(b->rx+n++*4096);
        if(assoc_fault==ASSOC_SESSION_END) {
            uint8_t *p=b->rx+n++*4096;memset(p,0,4096);p32(p,20);p[4]=0xfb;p[5]=3;p[7]=0x80;p32(p+12,1);
        }
        *(uint16_t *)(void *)b->status=n;return;
    }
    if(reg==0x460 && b->assoc_sent) {
        assert(v==b->command_count && d->master && b->assoc_received && b->assoc_tx_done);
        unsigned step=b->assoc_step;assert(step<3);
        const unsigned sizes[]={208,96,52},codes[]={9,10,8};
        uint8_t *cmd=b->command_data,*p=cmd+8,expected[208]={0};
        assert(cmd[0]==codes[step] && cmd[1]==3 && u32(cmd+4)==sizes[step]);
        unsigned aid=assoc_fault==ASSOC_MAX_AID?2007:7;
        if(step==0) {
            p32(expected,2);memcpy(expected+16,b->mac,6);p32(expected+24,0x17);p32(expected+28,1);
            p32(expected+40,0x15);p32(expected+44,1);p32(expected+48,1);p32(expected+136,100);
            if(assoc_fault==ASSOC_DTIM)p32(expected+140,300);
        } else if(step==1) {
            memcpy(expected+8,b->target.bssid,6);memcpy(expected+16,b->target.bssid,6);p32(expected+28,aid);
        } else {
            p32(expected+4,2);p32(expected+8,5);memcpy(expected+12,b->mac,6);
            p32(expected+20,assoc_fault==ASSOC_DTIM?4:12);expected[36]=1;expected[40]=aid;expected[41]=aid>>8;
        }
        assert(!memcmp(p,expected,sizes[step]));
        if(step==update_index && update_fault==1)return;
        unsigned n=b->consumed;uint8_t *packet=b->rx+n++*4096;memset(packet,0,4096);
        p32(packet,4);packet[4]=codes[step];packet[5]=3;packet[6]=v-1;
        if(step==update_index) {
            if(update_fault==2)packet[5]|=0x40;
            if(update_fault==3)packet[6]=99;
            if(update_fault==4){p32(packet,8);p32(packet+8,0xdeadbeef);}
        }
        *(uint16_t *)(void *)b->status=n;return;
    }
    auth_write(opaque,reg,v);
    if(reg==0x460 && v==0x10001) {
        if(assoc_fault==ASSOC_ALLOC)b->capacity=b->used;
        if(assoc_fault==ASSOC_DTIM)b->target.dtim_period=3;
    }
}
#ifndef WIFI_ASSOC_MAIN
#define WIFI_ASSOC_MAIN main
#endif
int WIFI_ASSOC_MAIN(int argc,char **argv) {
    assert(!wifi_auth_tests(argc,argv));
    FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
    blob=malloc(size);assert(blob && fread(blob,1,size,f)==(size_t)size);fclose(f);assert(!wifi_image_parse(blob,size,&image));
    f=fopen(argv[2],"rb");assert(f);fseek(f,0,SEEK_END);size=ftell(f);rewind(f);
    uint8_t *pnvm=malloc(size);assert(pnvm && fread(pnvm,1,size,f)==(size_t)size);fclose(f);
    image.pnvm=(WifiSection){pnvm,(unsigned)size};image.nvm_probe=image.scan_probe=image.auth_probe=image.assoc_probe=1;
    image.prepare_ssid_size=3;memcpy(image.prepare_ssid,"abc",3);
    WifiImage bad=image;bad.auth_probe=0;assert(wifi_assoc_validate(&bad));
    WifiBoot invalid={.pool=pool,.capacity=sizeof(pool)};Device untouched={.b=&invalid};
    WifiOps invalid_ops={&untouched,scan_read,assoc_write,write_byte,delay,mastering,prepare_log};
    assert(wifi_firmware_start(&invalid,&bad,&invalid_ops,0x441) && !untouched.writes && !invalid.attempted);
    auth_fault=AUTH_OK;scan_fault=SCAN_OK;fault_kind=0;expected_basic_ofdm=0x15;
    unsigned scenarios=0;
    for(beacon_group=2;beacon_group<=4;beacon_group+=2) {
        for(assoc_fault=0;assoc_fault<ASSOC_LAST;assoc_fault++) {
            WifiBoot b={.pool=pool,.capacity=sizeof(pool)};Device d={.b=&b,.fault=NVM_OPAQUE_STATUS};
            WifiOps ops={&d,scan_read,assoc_write,write_byte,delay,mastering,prepare_log};
            const char *error=wifi_firmware_start(&b,&image,&ops,0x441);
            int success=assoc_fault<ASSOC_NO_TX;
            if((error==0)!=success)fprintf(stderr,"assoc %u/%u: %s\n",beacon_group,assoc_fault,error?error:"unexpected success");
            assert((error==0)==success && b.quiesced && !d.master && b.auth_received && b.auth_tx_done);
            if(success)assert(b.assoc_complete && b.assoc_step==3 && b.assoc_tx_done && b.assoc_received && b.assoc_aid);
            else assert(!b.assoc_complete);
            ++scenarios;
        }
    }
    assoc_fault=ASSOC_OK;beacon_group=2;
    for(update_index=0;update_index<3;update_index++)for(update_fault=1;update_fault<=4;update_fault++) {
        WifiBoot b={.pool=pool,.capacity=sizeof(pool)};Device d={.b=&b,.fault=NVM_OPAQUE_STATUS};
        WifiOps ops={&d,scan_read,assoc_write,write_byte,delay,mastering,prepare_log};
        const char *error=wifi_firmware_start(&b,&image,&ops,0x441);
        if((error==0)!=(update_fault==4))fprintf(stderr,"update %u/%u: %s\n",update_index,update_fault,error?error:"unexpected success");
        assert((error==0)==(update_fault==4) && b.quiesced && !d.master);
        assert(b.assoc_complete==(update_fault==4));++scenarios;
    }
    /* Every truncated RX descriptor/body prefix stays bounded and cannot
     * claim acceptance; also exercise each complete, shortened frame prefix. */
    update_fault=0;assoc_fault=ASSOC_OK;
    uint8_t packet[4096];WifiBoot sample={.assoc_sent=1,.target={.channel=48,.rates=0xff0}};
    memcpy(sample.mac,"\x02\x03\x04\x05\x06\x07",6);
    memcpy(sample.target.bssid,"\x04\x05\x06\x07\x08\x09",6);
    assoc_response(packet,&sample);
    for(unsigned n=0;n<104;n++) {
        WifiBoot cut=sample;(void)wifi_assoc_mpdu(&cut,packet+8,n);assert(!cut.assoc_received);
    }
    for(unsigned n=0;n<40;n++) {
        WifiBoot cut=sample;packet[8]=n;(void)wifi_assoc_mpdu(&cut,packet+8,64+n);assert(!cut.assoc_received);
    }
    free(blob);free(pnvm);printf("AX211_ASSOC_HOST_OK %u transport/TX/AP/update scenarios; RF untested\n",scenarios);
    return 0;
}
