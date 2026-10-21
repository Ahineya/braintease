/* Actual startup + live RX/TX/key-command state, public independent crypto
 * vectors and fake DMA/MMIO. No RF or real credentials. */
#define WIFI_LIVE_MAIN existing_live_tests
#include "wifi-live.c"
#include "fixtures/wifi-rekey-vectors.h"
static unsigned command_hw,live_key_calls,reply_calls,command_fault;
static const uint8_t *expected_response,*expected_key;
static unsigned expected_id;
static void rekey_write(void *opaque,unsigned reg,uint32_t value) {
    Device *d=opaque;WifiBoot *b=d->b;
    if(b->live.active && reg==0x460 && value<65536) {
        command_hw=(command_hw+1)&65535;assert(value==command_hw);
        unsigned index=(command_hw-1)&255;uint8_t *desc=b->tx+index*256,*cmd=(void *)(uintptr_t)u64(desc+4),expected[88]={0};
        assert(b->rekey.stage==2 && b->command_pending && cmd==b->rekey.command_page);
        expected[0]=24;expected[1]=5;expected[2]=index;p32(expected+4,80);
        p32(expected+8,1);p32(expected+12,1);p32(expected+16,expected_id);
        p32(expected+20,beacon_group==2?0x43:0x42);
        memcpy(expected+24,expected_key,16);
        if(beacon_group==2) {
            memcpy(expected+40,expected_key+24,8);memcpy(expected+48,expected_key+16,8);
            memcpy(expected+56,expected_key+16,8);memcpy(expected+64,expected_key+24,8);
        }
        for(unsigned i=0;i<sizeof(expected);i++)if(cmd[i]!=expected[i]){fprintf(stderr,"public key command mismatch offset%u got%u expected%u group%u\n",i,cmd[i],expected[i],beacon_group);break;}
        assert(!memcmp(cmd,expected,sizeof(expected)));
        assert((u32(desc)&65535)==2 && (u32(desc)>>16)==20 && (u32(desc+12)&65535)==68);
        assert(u64(desc+14)==(uintptr_t)cmd+20);++live_key_calls;
        if(command_fault==3)return;
        uint8_t *p=next(b);p32(p,command_fault==4?3:8);p[4]=24;p[5]=5;p[6]=index;
        p32(p+8,0xdeadbeef); /* Upstream consumes opaque response payload. */
        if(command_fault==1)p[6]^=1;if(command_fault==2)p[5]|=0x40;
        publish(b,1);return;
    }
    live_write(opaque,reg,value);
    if(b->live.active && reg==0x460 && value>=65536 && b->rekey.stage==3) {
        unsigned slot=(hardware_write_pointer-1)&255;uint8_t *cmd=b->live.tx_pages+slot*4096;
        assert(expected_response && (u32(cmd+4)>>16)==1 && cmd[32]==8 && cmd[33]==0x41);
        assert(!memcmp(cmd+72,expected_response,99));++reply_calls;
    }
}
static WifiOps rekey_ops(Device *d){return (WifiOps){d,wpa_read,rekey_write,write_byte,delay,mastering,prepare_log};}
static void begin(WifiBoot *b,Device *d) {
    start_live(b,d);command_hw=b->command_count;live_key_calls=reply_calls=command_fault=0;
    expected_key=beacon_group==2?rekey_key_2:rekey_key_4;expected_response=beacon_group==2?rekey_g2_2:rekey_g2_4;expected_id=2;
}
static void group_message(WifiBoot *b,const uint8_t *body,unsigned size,unsigned protected,unsigned pn) {
    uint8_t *p=next(b);unsigned h=24,crypto=protected?8:0,len=h+crypto+8+size;
    p32(p,4+64+len);p[4]=0xc1;p[7]=0x80;
    uint8_t *desc=p+8,*f=desc+64;desc[0]=len;desc[1]=len>>8;p32(desc+12,protected?0x243:3);desc[42]=48;
    f[0]=8;f[1]=protected?0x42:2;memcpy(f+4,b->mac,6);memcpy(f+10,b->target.bssid,6);memcpy(f+16,vector_ap,6);
    if(protected){f[h]=pn;f[h+1]=pn>>8;f[h+3]=0x20;}
    const uint8_t snap[]={0xaa,0xaa,3,0,0,0,0x88,0x8e};memcpy(f+h+crypto,snap,8);memcpy(f+h+crypto+8,body,size);publish(b,1);
}
static void complete(WifiBoot *b,Device *d,unsigned status) {
    uint8_t *p=next(b);wpa_completion(p,hardware_write_pointer,0);p[8+40]=status;publish(b,1);
    WifiOps ops=rekey_ops(d);assert(!wifi_live_poll(b,&ops,1004));
    assert(!b->rekey.stage && !b->live.tx_count && b->live.active && !b->quiesced);
}
static void accept(WifiBoot *b,Device *d,const uint8_t *g1,unsigned n,unsigned protected,unsigned pn,unsigned install) {
    WifiOps ops=rekey_ops(d);unsigned before=live_key_calls;
    group_message(b,g1,n,protected,pn);assert(!wifi_live_poll(b,&ops,1000));
    assert(b->live.active && !b->quiesced && b->rekey.stage==(install?2U:4U));
    if(install)assert(!wifi_live_poll(b,&ops,1001));
    assert(b->rekey.stage==4 && live_key_calls==before+install);
    uint8_t data[42]={0};memcpy(data+6,b->mac,6);
    assert(wifi_live_tx(b,&ops,data,sizeof(data),1002)==NIC_WOULD_BLOCK);
    complete(b,d,1);
}
static void group_arp(WifiBoot *b,unsigned kid,unsigned pn) {
    uint8_t *p=next(b);arp(p,b,0);uint8_t *desc=p+8,*f=desc+64,*iv=f+24;
    memset(f+4,255,6);iv[3]=0x20|(kid<<6);
    if(beacon_group==2){iv[2]=pn;iv[0]=pn>>8;p32(desc+12,0x363);}
    else{iv[0]=pn;iv[1]=pn>>8;}
    p32(p,u32(p)|(1U<<26));publish(b,1);
}
static void group_rx(WifiBoot *b,Device *d,unsigned kid,unsigned pn,unsigned accepted) {
    uint8_t out[1514];unsigned length;WifiOps ops=rekey_ops(d);
    group_arp(b,kid,pn);assert(!wifi_live_poll(b,&ops,1005));
    assert(wifi_live_rx(b,out,sizeof(out),&length)==(accepted?NIC_OK:NIC_EMPTY));
}
static void rotations(void) {
    for(beacon_group=2;beacon_group<=4;beacon_group+=2)for(unsigned protected=0;protected<2;protected++) {
        WifiBoot b;Device d;begin(&b,&d);WifiOps ops=rekey_ops(&d);unsigned used=b.used;
        const uint8_t *g1=beacon_group==2?rekey_g1_2:rekey_g1_4;
        unsigned bytes=beacon_group==2?sizeof(rekey_g1_2):sizeof(rekey_g1_4);
        uint8_t ptk[48];memcpy(ptk,b.wpa.ptk,48);
        accept(&b,&d,g1,bytes,protected,1,1);
        assert(b.rekey.rotations==1 && b.rekey.keys[1].valid && b.rekey.keys[2].valid && !memcmp(b.wpa.ptk,ptk,48));
        group_rx(&b,&d,1,1,1);group_rx(&b,&d,2,17,0);group_rx(&b,&d,2,18,1);
        accept(&b,&d,g1,bytes,protected,2,0); /* exact retry, no reinstall */
        assert(b.rekey.keys[2].pn[0]==18 && b.rekey.rotations==1 && b.rekey.duplicates==1);
        /* Authenticated different material with the SAME replay counter is
         * not a new transaction, and cannot replace the installed GTK. */
        const uint8_t *changed=beacon_group==2?rekey_changed_same_replay_2:rekey_changed_same_replay_4;
        group_message(&b,changed,bytes,0,0);assert(!wifi_live_poll(&b,&ops,1005));
        assert(!b.rekey.stage && live_key_calls==1 && b.rekey.keys[2].pn[0]==18);
        expected_response=beacon_group==2?rekey_g2_new_replay_2:rekey_g2_new_replay_4;
        const uint8_t *same=beacon_group==2?rekey_same_key_new_replay_2:rekey_same_key_new_replay_4;
        accept(&b,&d,same,bytes,protected,3,0);
        assert(b.rekey.keys[2].pn[0]==18 && live_key_calls==1); /* authenticated RSC0 cannot reset */
        group_rx(&b,&d,2,18,0);group_rx(&b,&d,2,19,1);
        uint8_t before[sizeof(b.rekey.keys)];memcpy(before,b.rekey.keys,sizeof(before));
        const uint8_t *invalid[]={beacon_group==2?rekey_bad_wrap_2:rekey_bad_wrap_4,
            beacon_group==2?rekey_duplicate_2:rekey_duplicate_4,beacon_group==2?rekey_bad_length_2:rekey_bad_length_4};
        const unsigned sizes[]={bytes,beacon_group==2?sizeof(rekey_duplicate_2):sizeof(rekey_duplicate_4),bytes};
        /* These are old counters after replay10; rejection must be atomic. */
        for(unsigned i=0;i<3;i++){group_message(&b,invalid[i],sizes[i],0,0);assert(!wifi_live_poll(&b,&ops,1006));assert(!memcmp(before,b.rekey.keys,sizeof(before)) && !b.rekey.stage);}
        /* Wrap both independent hardware counters while recycling all slots. */
        b.command_count=command_hw=65534;b.live.tx_head=b.live.tx_tail=hardware_write_pointer=65534;
        const uint8_t *const *cycles=beacon_group==2?rekey_cycles_2:rekey_cycles_4;
        const uint8_t *const *replies=beacon_group==2?rekey_replies_2:rekey_replies_4;
        for(unsigned i=0;i<300;i++) {
            uint8_t key[32];unsigned n=beacon_group==2?32:16;
            key[0]=i/256;key[1]=i%256;for(unsigned j=0;j<n-2;j++)key[j+2]=j*23+i*7;
            expected_key=key;expected_id=1+i%2;expected_response=replies[i];
            accept(&b,&d,cycles[i],bytes,0,0,1);
            assert(b.used==used && b.live.active && b.rekey.rotations==i+2 && !memcmp(b.wpa.ptk,ptk,48));
        }
        assert(command_hw==298 && hardware_write_pointer==298 && live_key_calls==301);
        stop_live(&b,&d);for(unsigned i=0;i<sizeof(b.rekey);i++)assert(!((uint8_t *)&b.rekey)[i]);
    }
}
static void rejection(void) {
    beacon_group=2;unsigned bytes=sizeof(rekey_g1_2);
    const uint8_t *bad[]={rekey_bad_wrap_2,rekey_duplicate_2,rekey_bad_length_2,rekey_missing_2};
    const unsigned sizes[]={sizeof(rekey_bad_wrap_2),sizeof(rekey_duplicate_2),sizeof(rekey_bad_length_2),sizeof(rekey_missing_2)};
    for(unsigned fault=0;fault<12;fault++) {
        WifiBoot b;Device d;begin(&b,&d);WifiOps ops=rekey_ops(&d);
        uint8_t p[611];memcpy(p,rekey_g1_2,bytes);unsigned n=bytes;
        switch(fault) {
        case 0:p[81]^=1;break;case 1:p[5]&=~0x10;break;case 2:p[4]=254;break;
        case 3:p[9]=255;break; /* bigger replay without valid MIC */
        case 4:p[16]=7;break;case 5:p[97]=255;break;
        case 6:case 7:case 8:case 9:n=sizes[fault-6];memcpy(p,bad[fault-6],n);break;
        case 10:n=98;break;case 11:p[2]=255;break;
        }
        WifiGroupKey before[4];memcpy(before,b.rekey.keys,sizeof(before));
        group_message(&b,p,n,0,0);assert(!wifi_live_poll(&b,&ops,1000));
        assert(b.live.active && !b.rekey.stage && !live_key_calls && !reply_calls && !memcmp(before,b.rekey.keys,sizeof(before)));
        stop_live(&b,&d);
    }
    for(unsigned fault=1;fault<=4;fault++) {
        WifiBoot b;Device d;begin(&b,&d);WifiOps ops=rekey_ops(&d);command_fault=fault;
        group_message(&b,rekey_g1_2,bytes,1,1);assert(!wifi_live_poll(&b,&ops,1000));
        const char *error=wifi_live_poll(&b,&ops,fault==3?5001000:1001);
        assert(error && b.quiesced && !b.live.active && !reply_calls);
    }
    for(unsigned group=2;group<=4;group+=2) {
        beacon_group=group;WifiBoot b;Device d;begin(&b,&d);
        accept(&b,&d,group==2?rekey_g1_padded_2:rekey_g1_padded_4,
               group==2?sizeof(rekey_g1_padded_2):sizeof(rekey_g1_padded_4),1,1,1);stop_live(&b,&d);
    }
    beacon_group=2;
    /* A pending duplicate must not replace an outstanding command buffer. */
    WifiBoot b;Device d;begin(&b,&d);WifiOps ops=rekey_ops(&d);command_fault=3;
    group_message(&b,rekey_g1_2,bytes,0,0);assert(!wifi_live_poll(&b,&ops,1000));
    uint8_t owned[4096];memcpy(owned,b.rekey.command_page,4096);
    group_message(&b,rekey_g1_2,bytes,0,0);assert(!wifi_live_poll(&b,&ops,1001));
    assert(live_key_calls==1 && !memcmp(owned,b.rekey.command_page,4096));
    group_message(&b,rekey_same_key_new_replay_2,bytes,0,0);assert(!wifi_live_poll(&b,&ops,1002));
    assert(live_key_calls==1 && b.rekey.replay==9 && !memcmp(owned,b.rekey.command_page,4096));
    d.fault=STOP_TIMEOUT;assert(wifi_live_poll(&b,&ops,5001000));
    assert(!b.quiesced && !memcmp(owned,b.rekey.command_page,4096) && !memcmp(b.rekey.key,rekey_key_2,32));
    d.fault=NVM_OPAQUE_STATUS;stop_live(&b,&d);
    for(unsigned n=0;n<bytes;n++) {
        begin(&b,&d);ops=rekey_ops(&d);group_message(&b,rekey_g1_2,n,0,0);
        assert(!wifi_live_poll(&b,&ops,1000) && !b.rekey.stage && !live_key_calls);stop_live(&b,&d);
    }
    /* An authenticated original M3 retry only sends M4, never PTK/GTK ADD. */
    begin(&b,&d);ops=rekey_ops(&d);expected_response=vector_m4_2;
    accept(&b,&d,vector_m3_2,sizeof(vector_m3_2),0,0,0);assert(!live_key_calls && !b.rekey.rotations);stop_live(&b,&d);
    /* Full TX ring cannot be overwritten for G2. Reclaim it first, then send
     * the control reply through the same wrap-safe ownership discipline. */
    begin(&b,&d);ops=rekey_ops(&d);uint8_t eth[42]={0};memcpy(eth+6,b.mac,6);
    for(unsigned i=0;i<NIC_FRAMES;i++)assert(wifi_live_tx(&b,&ops,eth,sizeof(eth),900)==NIC_OK);
    group_message(&b,rekey_g1_2,bytes,0,0);assert(!wifi_live_poll(&b,&ops,1000));
    assert(!wifi_live_poll(&b,&ops,1001) && b.rekey.stage==3 && b.live.tx_count==NIC_FRAMES);
    uint8_t *p=next(&b);wpa_completion(p,hardware_write_pointer,0);publish(&b,1);
    assert(!wifi_live_poll(&b,&ops,1002) && b.rekey.stage==4 && b.live.tx_count==1);
    complete(&b,&d,1);stop_live(&b,&d);
    begin(&b,&d);ops=rekey_ops(&d);group_message(&b,rekey_g1_2,bytes,0,0);
    assert(!wifi_live_poll(&b,&ops,1000) && !wifi_live_poll(&b,&ops,1001));
    p=next(&b);wpa_completion(p,hardware_write_pointer,0);p[8+40]=0x82;publish(&b,1);
    assert(!wifi_live_poll(&b,&ops,1002) && b.live.active && b.rekey.stage==4 && live_key_calls==1 && reply_calls==2);
    complete(&b,&d,1);assert(b.rekey.rotations==1);stop_live(&b,&d);
    begin(&b,&d);ops=rekey_ops(&d);group_message(&b,rekey_g1_2,bytes,0,0);
    assert(!wifi_live_poll(&b,&ops,1000) && !wifi_live_poll(&b,&ops,1001));
    for(unsigned i=0;i<5;i++) {
        p=next(&b);wpa_completion(p,hardware_write_pointer,0);p[8+40]=0x82;publish(&b,1);
        const char *error=wifi_live_poll(&b,&ops,1002+i);
        if(i<4)assert(!error && b.live.active && live_key_calls==1);
        else assert(error && b.quiesced && !b.live.active);
    }
}
int main(int argc,char **argv) {
    assert(argc==3);
    FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);blob=malloc(size);assert(blob && fread(blob,1,size,f)==(size_t)size);fclose(f);assert(!wifi_image_parse(blob,size,&image));
    f=fopen(argv[2],"rb");assert(f);fseek(f,0,SEEK_END);size=ftell(f);rewind(f);uint8_t *pnvm=malloc(size);assert(pnvm && fread(pnvm,1,size,f)==(size_t)size);fclose(f);
    image.pnvm=(WifiSection){pnvm,(unsigned)size};image.nvm_probe=image.scan_probe=image.auth_probe=image.assoc_probe=image.wpa_probe=image.live=1;
    memcpy(image.pmk,vector_pmk,32);memcpy(image.snonce,vector_snonce,32);image.prepare_ssid_size=3;memcpy(image.prepare_ssid,"abc",3);
    auth_fault=AUTH_OK;assoc_fault=ASSOC_OK;scan_fault=SCAN_OK;wpa_fault=WPA_OK;key_fault=0;beacon_group=2;fault_kind=update_fault=0;expected_basic_ofdm=0x15;
    /* First assert is also the RED regression against the old link-stop path. */
    WifiBoot b;Device d;begin(&b,&d);WifiOps ops=rekey_ops(&d);
    group_message(&b,rekey_g1_2,sizeof(rekey_g1_2),1,1);const char *error=wifi_live_poll(&b,&ops,1000);
    if(error)fprintf(stderr,"GTK rotation incorrectly stopped link: %s\n",error);
    assert(!error && b.live.active && !b.quiesced);stop_live(&b,&d);
    rotations();rejection();free(blob);free(pnvm);
    puts("AX211_REKEY_HOST_OK protected/clear GTK, CCMP/TKIP, replay/MIC/wrap/KDE guards, duplicates without reinstall/PN reset, overlapping IDs,1200 rotations/queue wraps, fixed DMA, failures/timeouts; RF untested");
}
