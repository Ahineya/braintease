/* Exact production station-setup state machine with bounded native DMA mock.
 * No hardware/RF success is inferred from these tests. */
#define WIFI_SCAN_MAIN wifi_scan_tests
#include "wifi-scan.c"
static void p64(uint8_t *p,uint64_t v){for(unsigned n=0;n<8;n++)p[n]=v>>(8*n);}
static unsigned fault_step,fault_kind,setup_commands;
static unsigned beacon_group=4;
static unsigned expected_basic_ofdm=0x15;
static void prepare_log(void *opaque,const char *name,uint64_t v) {
    Device *d=opaque;scan_log(opaque,name,v);
    if(!strcmp(name,"AX211_TARGET_BASIC_RATES") && fault_kind>=11)
        d->b->capacity=d->b->used+(fault_kind==11?0:fault_kind==12?65536:69632);
}
static void secured_beacon(uint8_t *packet) {
    beacon(packet);uint8_t *d=packet+8,*f=d+64;
    static const uint8_t ies[]={0,3,'a','b','c',1,8,0x8c,18,0x98,36,0xb0,72,96,108,
        48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
    unsigned len=36+sizeof(ies);p32(packet,4+64+len);d[0]=len;d[42]=48;
    f[32]=100;f[34]=0x11;memcpy(f+36,ies,sizeof(ies));
    f[36+22]=beacon_group; /* actual RSN group selector type, not pairwise */
}
static void prepare_write(void *opaque,unsigned reg,uint32_t v) {
    Device *d=opaque;WifiBoot *b=d->b;
    if(reg!=0x460 || !b->prepare_started) {
        scan_write(opaque,reg,v);
        if(reg==0x460 && b->command_code==13) {
            unsigned producer=*(uint16_t *)(void *)b->status;
            for(unsigned n=b->consumed;n<producer;n++) {
                uint8_t *packet=b->rx+n*4096;
                if(packet[4]==0xc1 && packet[8+42]==48)secured_beacon(packet);
            }
        }
        return;
    }
    unsigned step=b->prepare_step;assert(step<7 && d->master);++setup_commands;
    const unsigned groups[7]={1,5,3,3,3,3,5},codes[7]={8,8,8,9,9,10,23};
    const unsigned sizes[7]={32,32,52,208,208,96,36};
    uint8_t *cmd=b->command_data,*p=cmd+8,*desc=b->tx+(v-1)*256;
    assert(cmd[0]==codes[step] && cmd[1]==groups[step] && cmd[2]==v-1 && !cmd[3]);
    assert(u32(cmd+4)==(sizes[step]|(step==1?2U<<24:0)));
    assert((u32(desc)&65535)==2 && u32(desc)>>16==20 && u64(desc+4)==(uintptr_t)cmd);
    assert((u32(desc+12)&65535)==sizes[step]-12 && u64(desc+14)==(uintptr_t)cmd+20);
    uint8_t expected[208]={0};
    switch(step) {
    case 0:p32(expected+4,1);p32(expected+8,48);break;
    case 1:p32(expected+4,0x1406);break;
    case 2:p32(expected+4,1);p32(expected+8,5);memcpy(expected+12,b->mac,6);p32(expected+20,12);break;
    case 3:p32(expected,1);memcpy(expected+16,b->mac,6);break;
    case 4:p32(expected,2);memcpy(expected+16,b->mac,6);p32(expected+24,3);p32(expected+28,1);
        p32(expected+40,expected_basic_ofdm);p32(expected+136,100);break;
    case 5:memcpy(expected+8,b->target.bssid,6);memcpy(expected+16,b->target.bssid,6);break;
    case 6:p32(expected+4,1);expected[8]=15;p32(expected+16,5);
        p64(expected+20,(uintptr_t)b->management_bc);p64(expected+28,(uintptr_t)b->management_tx);break;
    }
    assert(!memcmp(p,expected,sizes[step]));
    for(unsigned n=0;n<256*256;n++)assert(!b->management_tx[n]); /* NO frame submitted */
    for(unsigned n=0;n<4096;n++)assert(!b->management_bc[n]);
    if(step==fault_step && fault_kind==1)return;
    unsigned n=b->consumed;uint8_t *packet=b->rx+n++*4096;
    memset(packet,0,4096);p32(packet,step==6?12:4);packet[4]=cmd[0];packet[5]=cmd[1];packet[6]=v-1;
    if(step==6){packet[8]=1;packet[10]=1;} /* photographed raw queue flags */
    if(step==fault_step) {
        if(fault_kind==2)packet[5]|=0x40;
        if(fault_kind==3)packet[6]=99;
        if(fault_kind==4)p32(packet,step==6?11:8);
        if(fault_kind==5){packet=b->rx+n++*4096;country_notice(packet);}
        if(fault_kind==6){d->fault=RFKILL;d->reg[0x24/4]&=~0x08000000U;}
        if(fault_kind==7)packet[8]=2; /* wrong queue ID */
        if(fault_kind==8)packet[12]=1; /* wrong queue write pointer */
        if(fault_kind==9){packet[10]=255;packet[11]=255;} /* raw flags remain uninterpreted */
        if(fault_kind==10)packet[14]=1; /* reserved queue response */
    }
    *(uint16_t *)(void *)b->status=n;
}
static void security_checks(void) {
    uint8_t packet[4096];secured_beacon(packet);
    WifiBoot b={.scan_channels_5=8,.scan_acked=1,.scan_complete=1};
    assert(!wifi_scan_mpdu(&b,packet+8,(u32(packet)&0x3fff)-4) && b.scan_count==1);
    WifiAp good=b.aps[0];assert(good.akms==4 && good.pair_ciphers==16 && good.group_cipher==16);
    assert(good.rates==0xff0 && good.basic_rates==0x150 && good.beacon_interval==100);
    WifiImage fw={.prepare_ssid_size=3,.prepare_ssid={'a','b','c'}};
    assert(!wifi_target_select(&b,&fw) && b.target.channel==48);
    /* Never fall back to an open clone, WPA1/TKIP-unicast, SAE-only, PMF-required,
     * malformed RSN, non-infrastructure BSS or unknown required rates. */
    for(unsigned n=0;n<13;n++) {
        b.aps[0]=good;
        switch(n) {
        case 0:b.aps[0].privacy=0;break;
        case 1:b.aps[0].rsn=0;break;
        case 2:b.aps[0].group_cipher=1U<<8;break; /* unsupported GCMP group */
        case 3:b.aps[0].pair_ciphers=4;break;
        case 4:b.aps[0].akms=1U<<8;break;
        case 5:b.aps[0].rsn_caps=64;break;
        case 6:b.aps[0].unknown_basic=1;break;
        case 7:b.aps[0].basic_rates=0;break;
        case 8:b.aps[0].capabilities=0x12;break;
        case 9:b.aps[0].beacon_interval=0;break;
        case 10:b.aps[0].ssid_size=2;break;
        case 11:b.aps[0].ssid[0]='A';break;
        case 12:b.aps[0].details_valid=0;break;
        }
        assert(wifi_target_select(&b,&fw));
    }
    /* Mixed WPA2: CCMP unicast and TKIP group is eligible for this no-TX
     * setup checkpoint. It is not proof of implemented GTK/TKIP support. */
    beacon_group=2;secured_beacon(packet);
    WifiBoot mixed={.scan_channels_5=8,.scan_acked=1,.scan_complete=1};
    assert(!wifi_scan_mpdu(&mixed,packet+8,(u32(packet)&0x3fff)-4));
    assert(mixed.scan_count==1 && mixed.aps[0].details_valid && mixed.aps[0].group_cipher==4);
    assert(mixed.aps[0].group_suite==0x02ac0f00 && mixed.aps[0].pair_ciphers==16);
    assert(!wifi_target_select(&mixed,&fw));
    beacon_group=4;secured_beacon(packet);
    b.aps[0]=good;b.aps[0].energy=20;b.aps[0].privacy=0;
    b.aps[1]=good;b.aps[1].energy=80;b.aps[2]=good;b.aps[2].energy=40;b.scan_count=3;
    assert(!wifi_target_select(&b,&fw) && b.target.energy==40);
    b.reg_generation=1;assert(wifi_target_select(&b,&fw));b.reg_applied_generation=1;
    assert(!wifi_target_select(&b,&fw));b.command_pending=1;assert(wifi_target_select(&b,&fw));
    /* Every IE truncation is bounded. Complete prefixes with no RSN cannot
     * qualify; variable RSN counts/optional PMKIDs are independently checked. */
    uint8_t *ies=packet+8+64+36;unsigned bytes=packet[8]-36;
    for(unsigned n=0;n<bytes;n++) {
        WifiAp ap=good;ap.rates=ap.basic_rates=ap.akms=ap.pair_ciphers=ap.group_cipher=0;
        const char *error=wifi_ap_details(&ap,ies,n);
        assert(error || !ap.akms);
    }
    uint8_t copy[256];memcpy(copy,ies,bytes);copy[17+6]=255;copy[17+7]=255;
    WifiAp ap={0};assert(wifi_ap_details(&ap,copy,bytes));
    memcpy(copy,ies,bytes);memcpy(copy+bytes,ies+15,22);ap=(WifiAp){0};
    assert(wifi_ap_details(&ap,copy,bytes+22)); /* duplicate RSN */
    memcpy(copy,ies,bytes);copy[16]=22;copy[bytes]=1;copy[bytes+1]=0;ap=(WifiAp){0};
    assert(wifi_ap_details(&ap,copy,bytes+2)); /* PMKID count without bytes */
    /* Deterministic malformed-IE corpus under UBSan; no unbounded counts. */
    uint32_t random=0x31415926;
    for(unsigned n=0;n<20000;n++) {
        for(unsigned j=0;j<sizeof(copy);j++){random=random*1664525+1013904223;copy[j]=random>>24;}
        ap=(WifiAp){0};(void)wifi_ap_details(&ap,copy,n%257);
    }
}
#ifndef WIFI_PREPARE_MAIN
#define WIFI_PREPARE_MAIN main
#endif
int WIFI_PREPARE_MAIN(int argc,char **argv) {
    /* Actual HP allocation reply: queue1, raw flags1, initial pointer0.
     * Linux/OpenBSD allocation consumers do not use this legacy flag field. */
    uint8_t photographed[8]={1,0,1,0,0,0,0,0};
    WifiBoot reply={.prepare_started=1,.prepare_step=7};
    assert(!wifi_prepare_reply(&reply,photographed,sizeof(photographed)));
    assert(!wifi_scan_tests(argc,argv));security_checks();
    /* wifi_scan_tests released its firmware bytes; read fresh for this suite. */
    FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
    blob=malloc(size);assert(blob && fread(blob,1,size,f)==(size_t)size);fclose(f);
    assert(!wifi_image_parse(blob,size,&image));
    f=fopen(argv[2],"rb");assert(f);fseek(f,0,SEEK_END);size=ftell(f);rewind(f);
    uint8_t *pnvm=malloc(size);assert(pnvm && fread(pnvm,1,size,f)==(size_t)size);fclose(f);
    image.pnvm=(WifiSection){pnvm,(unsigned)size};image.nvm_probe=image.scan_probe=1;
    image.prepare_ssid_size=3;memcpy(image.prepare_ssid,"abc",3);assert(!wifi_prepare_validate(&image));
    for(unsigned n=0;n<9;n++) {
        WifiImage bad=image;
        switch(n) {
        case 0:bad.phy_version=3;break;case 1:bad.rlc_version=1;break;
        case 2:bad.mac_version=1;break;case 3:bad.link_version=1;break;
        case 4:bad.sta_version=2;break;case 5:bad.queue_version=0;break;
        case 6:bad.capabilities[3]&=~(1U<<14);break;
        case 7:bad.capabilities[1]&=~(1U<<16);break;case 8:bad.prepare_ssid_size=33;break;
        }
        WifiBoot b={.pool=pool,.capacity=sizeof(pool)};Device d={.b=&b};
        WifiOps ops={&d,scan_read,prepare_write,write_byte,delay,mastering,prepare_log};
        assert(wifi_firmware_start(&b,&bad,&ops,0x441) && !d.writes && !b.attempted);
    }
    unsigned scenarios=0;scan_fault=SCAN_OK;
    for(beacon_group=2;beacon_group<=4;beacon_group+=2) {
    for(fault_step=0;fault_step<7;fault_step++)for(fault_kind=0;fault_kind<=(fault_step==6?10U:6U);fault_kind++) {
        WifiBoot b={.pool=pool,.capacity=sizeof(pool)};Device d={.b=&b,.fault=NVM_OPAQUE_STATUS};setup_commands=0;
        WifiOps ops={&d,scan_read,prepare_write,write_byte,delay,mastering,prepare_log};
        const char *error=wifi_firmware_start(&b,&image,&ops,0x441);
        int success=fault_kind==0 || (fault_kind==4 && fault_step!=6) || (fault_step==6 && (fault_kind==9 || fault_kind==10));
        if((error==0)!=success)fprintf(stderr,"prepare %u/%u: %s\n",fault_step,fault_kind,error?error:"unexpected success");
        assert((error==0)==success && b.quiesced && !d.master);
        if(!error)assert(b.prepare_complete && b.management_qid==1 && b.prepare_step==7 && setup_commands==7 &&
            b.management_flags==(fault_kind==9?65535U:1U) && !b.auth_tx_done &&
            b.management_aux==(fault_step==6 && fault_kind==10?1U:0U));
        ++scenarios;
    }
    for(fault_kind=11;fault_kind<=13;fault_kind++) {
        WifiBoot b={.pool=pool,.capacity=sizeof(pool)};Device d={.b=&b,.fault=NVM_OPAQUE_STATUS};setup_commands=0;
        WifiOps ops={&d,scan_read,prepare_write,write_byte,delay,mastering,prepare_log};
        const char *error=wifi_firmware_start(&b,&image,&ops,0x441);
        assert(error && b.quiesced && !d.master && !setup_commands);++scenarios;
    }
    }
    /* Exercise repeated scan commands through the REAL startup dispatcher,
     * then ordinary station setup: first scan empty, second sees the target.
     * Permanently missing SSID stops after exactly three passive passes. */
    fault_kind=0;beacon_group=4;
    for(scan_fault=SCAN_TARGET_LATE;scan_fault<=SCAN_TARGET_ABSENT;scan_fault++) {
        WifiBoot b={.pool=pool,.capacity=sizeof(pool)};Device d={.b=&b,.fault=NVM_OPAQUE_STATUS};setup_commands=0;
        WifiOps ops={&d,scan_read,prepare_write,write_byte,delay,mastering,prepare_log};
        const char *error=wifi_firmware_start(&b,&image,&ops,0x441);
        assert(b.quiesced && !d.master);
        if(scan_fault==SCAN_TARGET_LATE)assert(!error && b.scan_attempts==2 && b.scan_uid==256 &&
            b.prepare_complete && setup_commands==7 && b.command_count==20);
        else assert(error && strstr(error,"bounded passive scans") && b.scan_attempts==3 &&
            b.scan_uid==512 && !b.prepare_started && !setup_commands && b.command_count==14);
        ++scenarios;
    }
    scan_fault=SCAN_OK;
    free(blob);free(pnvm);
    printf("AX211_PREPARE_HOST_OK RSN/target guards; %u station/queue transport scenarios; no RF acceptance\n",scenarios);
    return 0;
}
