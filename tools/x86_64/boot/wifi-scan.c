/* Bounded passive AX211 discovery, OpenBSD iwx1.230/reg1.75 wire layouts.
 * See wifi-LICENSE.txt. Native buffers only; no association or data TX. */
#include "wifi.h"
static unsigned u16(const uint8_t *p){return p[0]|(unsigned)p[1]<<8;}
static uint32_t u32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void p16(uint8_t *p,unsigned v){p[0]=v;p[1]=v>>8;}
static void p32(uint8_t *p,uint32_t v){for(unsigned n=0;n<4;n++)p[n]=v>>(8*n);}
static void p64(uint8_t *p,uint64_t v){for(unsigned n=0;n<8;n++)p[n]=v>>(8*n);}
static int country_valid(unsigned country) {
    unsigned hi=country>>8,lo=country&255;
    return (hi>='A' && hi<='Z' && lo>='A' && lo<='Z') || country==0x3030;
}
static int source_valid(unsigned source) {
    return (source<=9 && source!=6) || source==0x10 || source==0x11;
}
int wifi_mac_valid(const uint8_t *p) {
    static const uint8_t reserved[6]={2,0xcc,0xaa,0xff,0xee,0};
    unsigned nonzero=0,different=0;
    for(unsigned n=0;n<6;n++){nonzero|=p[n];different|=p[n]^reserved[n];}
    return nonzero && different && !(p[0]&1);
}
static void mac_read(WifiBoot *b,const WifiOps *o,unsigned base) {
    uint32_t a=o->read(o->opaque,base),c=o->read(o->opaque,base+4);
    for(unsigned n=0;n<4;n++)b->mac[n]=a>>(24-8*n);
    b->mac[4]=c>>8;b->mac[5]=c;
}
const char *wifi_scan_validate(const WifiImage *fw) {
    if(!fw->scan_probe)return 0;
    if(!fw->nvm_probe || !fw->pnvm.data)return "scan requires NVM profile";
    if(fw->scan_cfg_version!=5 || fw->scan_request_version!=17 ||
       fw->rx_notification!=5 || fw->mcc_notification!=6)return "unsupported scan/RX/MCC ABI";
    if(!(fw->api[1]&(1U<<24)) || !(fw->capabilities[0]&2) ||
       !(fw->capabilities[2]&(1U<<25)) || !(fw->capabilities[2]&(1U<<10)) ||
       (fw->capabilities[0]&(1U<<12)) ||
       (!(fw->api[0]&(1U<<9)) && !(fw->capabilities[0]&(1U<<29))))return "unsupported scan capabilities";
    return 0;
}
/* One fresh page per command, no reuse/wrap. First TB is at most20 bytes,
 * remainder in the second native TFH TB exactly as the upstream transport. */
const char *wifi_command(WifiBoot *b,const WifiOps *o,unsigned group,unsigned code,unsigned version,const uint8_t *p,unsigned bytes) {
    if(b->command_pending || b->command_count>=32 || bytes>4000)return "scan command ownership/size";
    uint8_t *cmd=wifi_dma_reserve(b,4096);
    if(!cmd)return "scan command DMA allocation";
    unsigned index=b->command_count;uint8_t *desc=b->tx+index*256;
    cmd[0]=code;cmd[1]=group;cmd[2]=index;p16(cmd+4,bytes);cmd[7]=version;
    for(unsigned n=0;n<bytes;n++)cmd[8+n]=p[n];
    unsigned total=bytes+8,first=total>20?20:total;
    p16(desc,total>20?2:1);p16(desc+2,first);p64(desc+4,(uintptr_t)cmd);
    if(total>20){p16(desc+12,total-20);p64(desc+14,(uintptr_t)cmd+20);}
    b->command_data=cmd;b->command_code=code;b->command_group=group;b->command_pending=1;
    ++b->command_count;++b->nvm_progress;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    o->write(o->opaque,0x460,b->command_count);
    o->log(o->opaque,"AX211_RADIO_COMMAND_SUBMITTED",group<<8|code);
    return 0;
}
static const char *scan_request(WifiBoot *b,uint8_t *p,unsigned uid) {
    if(!b->regulatory_done || !b->mac_valid || !(b->scan_channels|b->scan_channels_5))return "scan missing MAC/channel profile";
    /* v17=8+36+540+12+1344 bytes. No SSIDs/BSSIDs, force passive. */
    p32(p,uid);p32(p+4,6);uint8_t *gp=p+8,*cp=p+44,*period=p+584,*probe=p+596;
    p16(gp,(1U<<11)|(1U<<7)|6);gp[4]=gp[5]=10;
    gp[6]=2;gp[7]=8;gp[8]=10;p16(gp+10,300);p32(gp+28,6);
    gp[32]=gp[33]=110;cp[0]=1U<<5;cp[2]=10;cp[3]=2;
    for(unsigned n=0;n<7;n++) {
        unsigned channel=n<3?1+n*5:36+(n-3)*4;
        if(n<3?!(b->scan_channels&(1U<<channel)):!(b->scan_channels_5&(1U<<(n-3))))continue;
        uint8_t *ch=cp+4+cp[1]++*8;p32(ch,n<3?1U<<30:0); /* band1=2.4, band0=5 */
        ch[4]=channel;ch[5]=128;ch[6]=1;
    }
    period[2]=1; /* one iteration; no infinite schedule */
    p16(probe+2,26);p16(probe+4,26);p16(probe+6,16);p16(probe+8,42);p16(probe+10,10);p16(probe+16,52);
    uint8_t *frame=probe+20;frame[0]=0x40;
    for(unsigned n=0;n<6;n++){frame[4+n]=frame[16+n]=255;frame[10+n]=b->mac[n];}
    static const uint8_t rates[16]={1,8,0x82,0x84,0x8b,0x96,12,18,24,36,50,4,48,72,96,108};
    for(unsigned n=0;n<16;n++)frame[26+n]=rates[n];
    static const uint8_t rates5[10]={1,8,0x8c,18,0x98,36,0xb0,72,96,108};
    for(unsigned n=0;n<10;n++)frame[42+n]=rates5[n];
    return 0;
}
static int target_matches(const WifiBoot *b,const WifiAp *ap) {
    if(!b->scan_target_size || ap->ssid_size!=b->scan_target_size)return 0;
    for(unsigned n=0;n<ap->ssid_size;n++)if(ap->ssid[n]!=b->scan_target_ssid[n])return 0;
    return 1;
}
static int target_present(const WifiBoot *b) {
    for(unsigned n=0;n<b->scan_count;n++)if(target_matches(b,&b->aps[n]))return 1;
    return 0;
}
static const char *scan_submit(WifiBoot *b,const WifiOps *o) {
    if(b->scan_attempts>=WIFI_SCAN_ATTEMPTS_MAX)return "passive scan retry limit";
    /* Leave at least16 of queue0's32 entries for station7, session1,
     * association3, keys2 and the remaining bounded MCC refreshes. */
    if(b->scan_attempts && b->command_count>=16)return "passive rescan command reserve";
    /* v17 uid type occupies bits0..7; sequence starts at bit8. Keep type0,
     * as the pinned iwx foreground request, but distinguish later requests.
     * SCAN_COMPLETE returns this exact uid (iwxreg.h *_UID_SEQ_OFFSET). */
    unsigned uid=b->scan_attempts<<8;
    uint8_t data[1940]={0};const char *error=scan_request(b,data,uid);if(error)return error;
    error=wifi_command(b,o,1,13,0,data,sizeof(data));if(error)return error;
    b->scan_uid=uid;++b->scan_attempts;
    b->scan_started=1;b->scan_acked=b->scan_complete=b->scan_rescan_pending=0;
    b->scan_requested_channels=b->scan_channels;b->scan_requested_channels_5=b->scan_channels_5;
    /* Results belong to this listening pass, not an earlier BSSID/channel.
     * Cumulative malformed/drop counters remain available for diagnosis. */
    b->scan_count=0;
    o->log(o->opaque,"AX211_PASSIVE_SCAN_ATTEMPT",b->scan_attempts);
    o->log(o->opaque,"AX211_PASSIVE_SCAN_UID",uid);
    o->log(o->opaque,"AX211_PASSIVE_SCAN_CHANNEL_MASK",b->scan_channels);
    o->log(o->opaque,"AX211_PASSIVE_SCAN_5GHZ_MASK",b->scan_channels_5);
    return 0;
}
const char *wifi_scan_advance(WifiBoot *b,const WifiImage *fw,const WifiOps *o) {
    if(!fw->scan_probe || !b->nvm_complete || b->command_pending)return 0;
    if(!b->scan_started) {
        if(fw->prepare_ssid_size>sizeof(b->scan_target_ssid))return "passive scan target length";
        b->scan_target_size=fw->prepare_ssid_size;
        for(unsigned n=0;n<b->scan_target_size;n++)b->scan_target_ssid[n]=fw->prepare_ssid[n];
    }
    if(b->radio_step>=9) {
        if(b->prepare_started || !b->scan_complete || !b->scan_acked)return 0;
        if(b->reg_generation==b->reg_applied_generation) {
            /* Profile expansion is not evidence the new channels were heard.
             * Initial target absence gets at most two fresh passive passes;
             * an observed but incompatible target still fails security setup. */
            if(b->scan_rescan_pending)return scan_submit(b,o);
            if(b->scan_target_size && !target_present(b)) {
                if(b->scan_attempts>=WIFI_SCAN_ATTEMPTS_MAX)
                    return "target SSID not found after bounded passive scans";
                return scan_submit(b,o);
            }
            return 0;
        }
        if(b->reg_refreshes>=2)return "regulatory refresh limit";
        /* CHUB requests applying its detected MCC, not querying ZZ again.
         * Follow Linux iwl_mvm_rx_chub_update_mcc -> update_mcc: exact
         * firmware country/source, then trust only the returned profile.
         * Initial startup still queries ZZ/GET_CURRENT; no host override. */
        uint8_t current[28]={0};p16(current,b->reg_country);current[2]=b->reg_source;
        b->reg_query_generation=b->reg_generation;
        ++b->reg_refreshes;
        o->log(o->opaque,"AX211_REG_PROFILE_APPLY",b->reg_refreshes);
        return wifi_command(b,o,1,0xc8,0,current,sizeof(current));
    }
    if(!b->mac_valid) {
        mac_read(b,o,0x388); /* NIC lock already held by startup transport. */
        if(!wifi_mac_valid(b->mac))mac_read(b,o,0x380);
        if(!wifi_mac_valid(b->mac))return "no valid strap/OTP MAC address";
        b->mac_valid=1;o->log(o->opaque,"AX211_MAC_ADDRESS_VALID",1);
    }
    uint8_t data[1940]={0};unsigned group=1,code=0,bytes=4;
    switch(b->radio_step) {
    case 0:code=0x98;p32(data,b->nvm_tx_chains);break;
    case 1:code=0x9b;bytes=8;p32(data,3);break; /* WIFI coexistence, as iwx */
    case 2:group=2;code=1;bytes=8;break; /* integrated MA, no xtal delay */
    case 3:group=4;code=4;bytes=20;break; /* firmware thermal kill responsibility */
    case 4:code=0x77;break; /* power saving disabled */
    case 5:code=0xc8;bytes=28;data[0]=data[1]='Z';data[2]=0x10;
        /* GET_CURRENT does not apply an earlier CHUB notification.
         * Keep any pre-query detection pending for the post-scan update. */
        b->reg_query_generation=0;break;
    case 6:code=12;bytes=12;p32(data+4,b->nvm_tx_chains);p32(data+8,b->nvm_rx_chains);break;
    case 7:code=0xd2;bytes=60;break; /* disable beacon filtering */
    case 8:{const char *error=scan_submit(b,o);if(!error)++b->radio_step;return error;}
    default:return "scan state";
    }
    const char *error=wifi_command(b,o,group,code,0,data,bytes);
    if(!error)++b->radio_step;
    return error;
}
static int channel_allowed(const WifiBoot *b,unsigned channel) {
    return channel<=13?!!(b->scan_channels&(1U<<channel)):
        channel>=36 && channel<=48 && !(channel%4)?!!(b->scan_channels_5&(1U<<((channel-36)/4))):0;
}
const char *wifi_scan_reply(WifiBoot *b,const uint8_t *p,unsigned bytes) {
    if(b->wpa.key_step)return wifi_wpa_reply(b,p,bytes);
    if(b->assoc_step)return wifi_assoc_reply(b,p,bytes);
    if(b->auth_enabled)return wifi_auth_reply(b,p,bytes);
    if(b->prepare_started)return wifi_prepare_reply(b,p,bytes);
    if(b->command_code==0xc8) {
        if(bytes<20 || u32(p+16)>110 || bytes!=20+4*u32(p+16))return "MCC reply bounds";
        /* Linux update_mcc/get_regdomain consumes the returned profile even
         * for ILLEGAL/LOW_PRIORITY status; status alone grants/denies nothing.
         * Keep it raw and use actual per-channel VALID and SKU information. */
        unsigned channels=u32(p+16);if(channels<11)return "MCC channel profile too short";
        /* Linux normalizes an all-zero firmware MCC to world (00).
         * Applied country may differ from the requested one: never invent
         * permissions from the request or from a beacon alone. */
        unsigned country=u16(p+4);if(!country)country=0x3030;
        if(!country_valid(country) || !source_valid(p[12]))return "MCC country/source";
        /* API6 uses the public v4 layout. Linux iwl_mvm_update_mcc and
         * OpenBSD iwx_send_update_mcc_cmd do not require RX reserved[3]==0.
         * HP returns03 in byte13. Preserve these octets as opaque diagnostics;
         * they grant no permission and never change channel/profile offsets. */
        b->reg_profile_aux=(unsigned)p[13]|(unsigned)p[14]<<8|(unsigned)p[15]<<16;
        b->reg_applied_country=country;b->reg_applied_source=p[12];
        b->reg_profile_status=u32(p);
        for(unsigned n=0;n<18;n++)b->mcc_channel_flags[n]=n<channels?u32(p+20+4*n):0;
        unsigned previous_channels=b->scan_channels,previous_5=b->scan_channels_5;
        b->scan_channels=b->scan_channels_5=0;
        if(b->nvm_mac_flags&1)for(unsigned n=0;n<3;n++) {
            unsigned ch=1+n*5;
            if(ch<=b->nvm_channels && (u32(p+20+4*(ch-1))&1) &&
               (b->nvm_channel_flags[ch-1]&1))b->scan_channels|=1U<<ch;
        }
        /* Both public v3/v4 maps put36/40/44/48 at profile indices14..17.
         * Short replies do not imply that missing channels are permitted. */
        if(b->nvm_mac_flags&2)for(unsigned n=0;n<4;n++) {
            unsigned index=14+n;
            if(index<channels && index<b->nvm_channels && (u32(p+20+4*index)&1) &&
               (b->nvm_channel_flags[index]&1))b->scan_channels_5|=1U<<n;
        }
        if(!(b->scan_channels|b->scan_channels_5))return "no allowed passive test channels";
        if(b->scan_started && ((b->scan_channels&~previous_channels) || (b->scan_channels_5&~previous_5)))
            b->scan_rescan_pending=1;
        b->regulatory_done=1;
        b->reg_applied_generation=b->reg_query_generation;
        unsigned kept=0;
        for(unsigned n=0;n<b->scan_count;n++) {
            if(channel_allowed(b,b->aps[n].channel))b->aps[kept++]=b->aps[n];
            else ++b->reg_filtered;
        }
        b->scan_count=kept;
    }
    if(b->command_code==13 && b->command_group==1)b->scan_acked=1;
    return 0;
}
const char *wifi_scan_mpdu(WifiBoot *b,const uint8_t *p,unsigned bytes) {
    if(bytes<64 || u16(p)>bytes-64)return "scan MPDU descriptor bounds";
    ++b->scan_frames;
    if((u32(p+12)&3)!=3)return 0; /* corrupt over-air frame, discard */
    unsigned len=u16(p),padding=(p[3]&0x20)?2:0;
    const uint8_t *frame=p+64;
    if(len<36+padding || (frame[0]!=0x80 && frame[0]!=0x50) || (frame[1]&0x43))return 0;
    if(!wifi_mac_valid(frame+16))return 0;
    unsigned channel=p[42];
    if(!channel_allowed(b,channel))return 0;
    WifiAp ap={.channel=channel,.privacy=!!(u16(frame+34+padding)&16)};
    ap.capabilities=u16(frame+34+padding);ap.beacon_interval=u16(frame+32+padding);
    for(unsigned n=0;n<6;n++)ap.bssid[n]=frame[16+n];
    unsigned a=p[40],c=p[41];ap.energy=!a?c:!c?a:a<c?a:c;
    unsigned ssid=0;
    for(unsigned at=36+padding;at<len;) {
        if(len-at<2 || frame[at+1]>len-at-2){++b->scan_bad_frames;return 0;}
        unsigned id=frame[at],n=frame[at+1];at+=2;
        if(id==0) {
            if(ssid++ || n>32){++b->scan_bad_frames;return 0;}
            ap.ssid_size=n;for(unsigned j=0;j<n;j++)ap.ssid[j]=frame[at+j];
        }
        if(id==48)ap.rsn=1; /* RSN IE present, not a security negotiation. */
        at+=n;
    }
    if(!ssid){++b->scan_bad_frames;return 0;}
    /* Detailed security/rates are required only by the explicit station-setup
     * profile. Discovery remains compatible with unusual/malformed RSN IEs. */
    if(wifi_ap_details(&ap,frame+36+padding,len-36-padding)) {
        ap.rates=0;ap.akms=0; /* cannot be selected for station setup */
    }
    unsigned index=0;
    for(;index<b->scan_count;index++) {
        unsigned equal=1;for(unsigned n=0;n<6;n++)if(ap.bssid[n]!=b->aps[index].bssid[n])equal=0;
        if(equal && ap.channel==b->aps[index].channel)break;
    }
    if(index==32) {
        /* A configured SSID arriving after32 neighbours must remain eligible
         * for the unchanged security selector. Never evict a target for an
         * unrelated SSID. Among equal classes retain the strongest signal;
         * zero energy is unknown and is considered weakest. */
        if(target_matches(b,&ap)) {
            for(unsigned n=0;n<32;n++)if(!target_matches(b,&b->aps[n])) {
                if(index==32 || !b->aps[n].energy ||
                   (b->aps[index].energy && b->aps[n].energy>b->aps[index].energy))index=n;
            }
            if(index==32) {
                for(unsigned n=0;n<32;n++)
                    if(index==32 || !b->aps[n].energy ||
                       (b->aps[index].energy && b->aps[n].energy>b->aps[index].energy))index=n;
                if(!ap.energy || (b->aps[index].energy && ap.energy>=b->aps[index].energy))index=32;
            }
        }
        ++b->scan_dropped;
        if(index==32)return 0;
    }
    if(index==b->scan_count)++b->scan_count;
    if(!ap.ssid_size && b->aps[index].ssid_size) {
        ap.ssid_size=b->aps[index].ssid_size;
        for(unsigned n=0;n<ap.ssid_size;n++)ap.ssid[n]=b->aps[index].ssid[n];
    }
    b->aps[index]=ap;
    return 0;
}
/* p is the four-byte RX header followed by its bounded payload. */
const char *wifi_scan_notification(WifiBoot *b,const uint8_t *p,unsigned bytes) {
    if(bytes<4)return "scan notification header";
    const char *auth_error=wifi_auth_notification(b,p,bytes);if(auth_error)return auth_error;
    unsigned code=p[0],group=p[1];
    if(group==4 && code==0xfe)return "radio thermal kill notification";
    if(group==0 && code==0xc9) {
        /* Country detection is expected during passive RX (iwx_mcc_update).
         * Finish listening, then apply the detected country/source and accept
         * only the returned channel profile (Linux CHUB update path).
         * Notifications do not reset command/scan deadlines. */
        if(bytes!=8)return "CHUB country notification size";
        unsigned country=u16(p+4);
        if(!country_valid(country))
            return "CHUB country code";
        if(!source_valid(p[6]))return "CHUB source"; /* RX padding is ignored upstream. */
        if(b->prepare_started)return "regulatory change during station setup";
        if(b->reg_notifications>=8)return "CHUB country notification limit";
        b->reg_country=country;b->reg_source=p[6];++b->reg_notifications;++b->reg_generation;
        return 0;
    }
    if(group!=0)return 0;
    if((code==15 || code==0xb5) && !b->scan_started)return "scan notification before submission";
    if(!b->scan_started)return 0;
    if(code==0xc1) {
        const char *error=wifi_auth_mpdu(b,p+4,bytes-4);if(error)return error;
        error=wifi_assoc_mpdu(b,p+4,bytes-4);if(error)return error;
        error=wifi_wpa_mpdu(b,p+4,bytes-4);if(error)return error;
        error=wifi_data_mpdu(b,p+4,bytes-4);if(error)return error;
        return wifi_scan_mpdu(b,p+4,bytes-4);
    }
    if(code==15) {
        if(bytes!=20)return "scan complete bounds/UID/state";
        unsigned uid=u32(p+4);
        if(uid!=b->scan_uid) {
            /* An earlier sequence can arrive after submission of the next
             * pass. It neither completes that pass nor extends its deadline.
             * Unknown/future/type-mismatched ids remain hard failures. */
            if(!(uid&255) && uid<b->scan_uid)return 0;
            return "scan complete bounds/UID/state";
        }
        if(b->scan_complete)return 0; /* duplicate current completion */
        if(p[10]!=1)return "passive scan aborted/status";
        b->scan_complete=1;++b->nvm_progress;
    }
    return 0;
}
