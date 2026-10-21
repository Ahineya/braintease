/* Included by the existing scan suite, so every enclosing station/live suite
 * exercises the same bounded rescan protocol. No firmware keys or RF traffic. */
typedef struct { WifiBoot *b; unsigned writes,last_code,last_uid; } RecoveryFixture;
static uint32_t recovery_read(void *opaque,unsigned reg) {(void)opaque;(void)reg;assert(0);return 0;}
static void recovery_write(void *opaque,unsigned reg,uint32_t value) {
    RecoveryFixture *f=opaque;WifiBoot *b=f->b;
    assert(reg==0x460 && value==b->command_count && value<=32);
    f->writes++;f->last_code=b->command_code;
    if(b->command_code==13) {
        const uint8_t *p=b->command_data+8;
        f->last_uid=u32(p);
        /* Public v17 type0/sequence-at-bit8, unchanged force-passive flags,
         * and exactly one iteration in each independently identified scan. */
        assert(!(f->last_uid&255) && (p[8]|(unsigned)p[9]<<8)==0x886 && p[586]==1);
        assert(p[45]>0 && p[45]<=7);
        for(unsigned n=0;n<p[45];n++) {
            unsigned c=p[52+8*n];
            assert(c==1 || c==6 || c==11 || c==36 || c==40 || c==44 || c==48);
        }
    }
}
static void recovery_log(void *opaque,const char *name,uint64_t value) {(void)opaque;(void)name;(void)value;}
static void recovery_new(WifiBoot *b,RecoveryFixture *f,WifiOps *ops) {
    *b=(WifiBoot){.pool=pool,.capacity=sizeof(pool),.nvm_complete=1,.mac_valid=1,
        .radio_step=8,.regulatory_done=1,.scan_channels=(1U<<1)|(1U<<6)|(1U<<11),
        .scan_channels_5=15,.nvm_mac_flags=3,.nvm_channels=18,.command_count=11};
    b->tx=wifi_dma_reserve(b,256*256);assert(b->tx);
    for(unsigned n=0;n<18;n++)b->nvm_channel_flags[n]=1;
    *f=(RecoveryFixture){.b=b};
    *ops=(WifiOps){f,recovery_read,recovery_write,0,0,0,recovery_log};
}
static void recovery_ack(WifiBoot *b) {
    assert(b->command_pending && b->command_code==13);
    assert(!wifi_scan_reply(b,0,0));b->command_pending=0;
}
static const char *recovery_complete(WifiBoot *b,unsigned uid) {
    uint8_t p[20]={15,0,0,0x80};p32(p+4,uid);p[10]=1;
    return wifi_scan_notification(b,p,sizeof(p));
}
static void recovery_profile(WifiBoot *b,unsigned low,unsigned high) {
    uint8_t p[20+18*4]={0};p[4]='L';p[5]='A';p[12]=5;p32(p+16,18);
    for(unsigned n=0;n<3;n++)if(low&(1U<<(1+5*n)))p32(p+20+(1+5*n-1)*4,1);
    for(unsigned n=0;n<4;n++)if(high&(1U<<n))p32(p+20+(14+n)*4,1);
    b->command_code=0xc8;b->command_group=1;
    assert(!wifi_scan_reply(b,p,sizeof(p)));b->command_pending=0;
}
static void recovery_beacon(WifiBoot *b,unsigned id,int configured,unsigned channel,unsigned energy) {
    uint8_t p[4096];beacon(p);p[8+42]=channel;p[8+40]=p[8+41]=energy;
    p[8+64+20]=id>>8;p[8+64+21]=id;
    if(!configured)p[8+64+38]='x';
    assert(!wifi_scan_mpdu(b,p+8,107));
}
static unsigned recovery_targets(const WifiBoot *b) {
    unsigned found=0;
    for(unsigned n=0;n<b->scan_count;n++)
        if(b->aps[n].ssid_size==3 && !memcmp(b->aps[n].ssid,"abc",3))++found;
    return found;
}
static void recovery_checks(void) {
    WifiImage fw={.scan_probe=1,.prepare_ssid_size=3,.prepare_ssid={'a','b','c'}};
    WifiBoot b;RecoveryFixture f;WifiOps ops;
    recovery_new(&b,&f,&ops);b.scan_channels_5=0;
    assert(!wifi_scan_advance(&b,&fw,&ops) && b.scan_attempts==1 && b.scan_uid==0);
    recovery_ack(&b);assert(!recovery_complete(&b,0));
    /* A new regulatory profile enables48 after the first completed scan.
     * Even an already-seen matching SSID on6 cannot skip the new pass. */
    recovery_beacon(&b,1,1,6,50);
    uint8_t notice[8]={0xc9,0,0,0x80,0x4c,0x41,5,0};
    assert(!wifi_scan_notification(&b,notice,sizeof(notice)));
    assert(!wifi_scan_advance(&b,&fw,&ops) && f.last_code==0xc8 && b.reg_refreshes==1);
    recovery_profile(&b,b.scan_channels,15);
    assert(b.scan_rescan_pending && b.reg_generation==b.reg_applied_generation);
    assert(!wifi_scan_advance(&b,&fw,&ops) && b.scan_attempts==2 && b.scan_uid==256 && !b.scan_count);
    assert(b.scan_requested_channels_5==15 && !b.scan_complete && !b.scan_acked);
    unsigned progress=b.nvm_progress;
    assert(!recovery_complete(&b,0) && !b.scan_complete && b.nvm_progress==progress);
    assert(recovery_complete(&b,512) && recovery_complete(&b,99));
    uint8_t short_complete[19]={15};assert(wifi_scan_notification(&b,short_complete,sizeof(short_complete)));
    recovery_beacon(&b,2,1,48,55);
    assert(!recovery_complete(&b,256) && b.scan_complete && !b.scan_acked);
    assert(!wifi_scan_advance(&b,&fw,&ops) && b.scan_attempts==2); /* ACK still owns queue. */
    recovery_ack(&b);progress=b.nvm_progress;
    assert(!recovery_complete(&b,256) && b.nvm_progress==progress);
    assert(!wifi_scan_advance(&b,&fw,&ops) && b.scan_attempts==2 && recovery_targets(&b)==1);
    /* Disabling48 drops stale results, and re-enabling48 schedules a fresh
     * pass even though that channel appeared in an older request. */
    recovery_profile(&b,b.scan_channels,7);assert(!b.scan_count && !b.scan_rescan_pending);
    recovery_beacon(&b,3,1,48,30);assert(!b.scan_count);
    recovery_profile(&b,b.scan_channels,15);assert(b.scan_rescan_pending);
    assert(!wifi_scan_advance(&b,&fw,&ops) && b.scan_attempts==3 && b.scan_uid==512);
    recovery_ack(&b);assert(!recovery_complete(&b,512));
    recovery_profile(&b,b.scan_channels,7);recovery_profile(&b,b.scan_channels,15);
    unsigned writes=f.writes,used=b.used,commands=b.command_count;
    assert(wifi_scan_advance(&b,&fw,&ops));
    assert(f.writes==writes && b.used==used && b.command_count==commands && b.scan_attempts==3);
    /* Plain target absence is exactly three passes, not three additional
     * passes. Non-target beacons never postpone the cap or authorize setup. */
    recovery_new(&b,&f,&ops);
    for(unsigned n=0;n<3;n++) {
        assert(!wifi_scan_advance(&b,&fw,&ops) && b.scan_attempts==n+1);
        recovery_beacon(&b,n,0,48,55);recovery_ack(&b);assert(!recovery_complete(&b,n<<8));
    }
    assert(strstr(wifi_scan_advance(&b,&fw,&ops),"bounded passive scans"));
    assert(f.writes==3 && b.command_count==14 && !b.prepare_started);
    /* Existing but incompatible SSIDs do not trigger security retries. */
    recovery_beacon(&b,77,1,48,40);
    assert(!wifi_scan_advance(&b,&fw,&ops) && f.writes==3);
    assert(wifi_target_select(&b,&fw)); /* Missing rates/RSN details are not accepted. */
    /* Queue headroom and DMA allocation failure do not publish a new pass. */
    recovery_new(&b,&f,&ops);assert(!wifi_scan_advance(&b,&fw,&ops));recovery_ack(&b);
    assert(!recovery_complete(&b,0));b.command_count=16;used=b.used;
    assert(strstr(wifi_scan_advance(&b,&fw,&ops),"command reserve"));
    assert(b.scan_attempts==1 && b.used==used && f.writes==1 && !b.command_pending);
    b.command_count=12;b.capacity=b.used;
    assert(strstr(wifi_scan_advance(&b,&fw,&ops),"DMA allocation"));
    assert(b.scan_attempts==1 && b.used==used && b.command_count==12 && f.writes==1);
    /* Discovery-only profiles do not repeat empty scans. */
    fw.prepare_ssid_size=0;recovery_new(&b,&f,&ops);
    assert(!wifi_scan_advance(&b,&fw,&ops));recovery_ack(&b);assert(!recovery_complete(&b,0));
    assert(!wifi_scan_advance(&b,&fw,&ops) && f.writes==1);
    /* Target admission is independent of how many unrelated BSSIDs arrived
     * first. Unrelated overflow never displaces the admitted target. */
    fw.prepare_ssid_size=3;recovery_new(&b,&f,&ops);assert(!wifi_scan_advance(&b,&fw,&ops));
    for(unsigned n=0;n<40;n++)recovery_beacon(&b,n,0,48,60);
    assert(b.scan_count==32 && !recovery_targets(&b) && b.scan_dropped==8);
    recovery_beacon(&b,100,1,48,70);assert(b.scan_count==32 && recovery_targets(&b)==1);
    for(unsigned n=101;n<140;n++)recovery_beacon(&b,n,0,48,20);
    assert(recovery_targets(&b)==1 && b.scan_count==32);
    /* A table containing only target BSSIDs retains the stronger candidates. */
    b.scan_count=0;b.scan_dropped=0;
    for(unsigned n=0;n<32;n++)recovery_beacon(&b,n,1,48,90);
    recovery_beacon(&b,100,1,48,30);assert(b.scan_count==32 && recovery_targets(&b)==32);
    unsigned strong=0;for(unsigned n=0;n<32;n++)strong+=b.aps[n].energy==30;assert(strong==1);
    recovery_beacon(&b,101,1,48,99);
    for(unsigned n=0;n<32;n++)assert(b.aps[n].energy!=99);
    puts("AX211_SCAN_RECOVERY_HOST_OK MCC enable/disable/rescan, UID/stale/duplicate/ACK ordering, exact3-pass cap, queue/DMA bounds, target saturation; RF untested");
}
