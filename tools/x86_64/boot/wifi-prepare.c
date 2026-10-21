/* MA/GF API89 station setup, following OpenBSD iwx1.230/reg1.75 MLD path.
 * See wifi-LICENSE.txt. This checkpoint allocates firmware contexts/queue but
 * transmits NO management/data frames and installs NO keys. Stop before shell. */
#include "wifi.h"
static unsigned u16(const uint8_t *p){return p[0]|(unsigned)p[1]<<8;}
static void p32(uint8_t *p,uint32_t v){for(unsigned n=0;n<4;n++)p[n]=v>>(8*n);}
static void p64(uint8_t *p,uint64_t v){for(unsigned n=0;n<8;n++)p[n]=v>>(8*n);}
const char *wifi_prepare_validate(const WifiImage *fw) {
    if(!fw->prepare_ssid_size)return 0;
    if(fw->prepare_ssid_size>32 || !fw->scan_probe)return "station setup requires SSID and scan";
    /* Pin the exact MA firmware interfaces inspected alongside this driver.
     * Missing STA_CONFIG version means v1, matching iwx_mld_add_sta_cmd. */
    if(!(fw->capabilities[3]&(1U<<14)) || !(fw->capabilities[1]&(1U<<16)) ||
       fw->phy_version!=4 || fw->rlc_version!=2 || fw->mac_version!=2 ||
       fw->link_version!=2 || fw->sta_version || fw->queue_version!=3)
        return "unsupported station PHY/RLC/MLD/queue ABI";
    return 0;
}
static void local(uint8_t *p,const WifiBoot *b){for(unsigned n=0;n<6;n++)p[n]=b->mac[n];}
const char *wifi_prepare_advance(WifiBoot *b,const WifiImage *fw,const WifiOps *o) {
    if(!fw->prepare_ssid_size || b->prepare_complete || b->command_pending ||
       !b->scan_complete || !b->scan_acked || b->reg_generation!=b->reg_applied_generation)return 0;
    if(!b->prepare_started) {
        const char *error=wifi_target_select(b,fw);
        if(!b->target.ssid_size)return error;
        o->log(o->opaque,"AX211_TARGET_CHANNEL",b->target.channel);
        o->log(o->opaque,"AX211_TARGET_RSN_GROUP",b->target.group_cipher);
        o->log(o->opaque,"AX211_TARGET_RSN_PAIR",b->target.pair_ciphers);
        o->log(o->opaque,"AX211_TARGET_RSN_AKM",b->target.akms);
        o->log(o->opaque,"AX211_TARGET_RSN_CAPS",b->target.rsn_caps);
        o->log(o->opaque,"AX211_TARGET_BASIC_RATES",b->target.basic_rates);
        if(error)return error;
        /* First checkpoint supports 5GHz OFDM/20MHz station setup only. */
        if(b->target.channel<36 || b->target.channel>48 || b->target.channel%4)
            return "station setup requires allowed low-band 5GHz target";
        if(!(b->scan_channels_5&(1U<<((b->target.channel-36)/4))))return "target channel no longer permitted";
        b->prepare_started=1;
        b->management_tx=wifi_dma_reserve(b,256*256);
        b->management_bc=wifi_dma_reserve(b,4096);
        if(!b->management_tx || !b->management_bc)return "management queue DMA allocation";
    }
    uint8_t data[208]={0};unsigned group=3,code=0,version=0,bytes=0;
    switch(b->prepare_step) {
    case 0: /* UHB PHY_CONTEXT v3/v4, 20MHz, primary channel, main LMAC0. */
        group=1;code=8;bytes=32;p32(data+4,1);p32(data+8,b->target.channel);break;
    case 1: /* RLC_CONFIG v2: PHY0, actual RX antennas, one idle/active chain. */
        group=5;code=8;version=2;bytes=32;p32(data+4,(b->nvm_rx_chains<<1)|(1U<<10)|(1U<<12));break;
    case 2: /* MLD MAC0, ADD BSS_STA, unassociated, group+beacon filter. */
        code=8;bytes=52;p32(data+4,1);p32(data+8,5);local(data+12,b);p32(data+20,12);break;
    case 3: /* MLD LINK0 ADD on MAC0/PHY0. */
        code=9;bytes=208;p32(data,1);local(data+16,b);break;
    case 4: /* MLD LINK0 MODIFY active + legacy basic rates. */
        code=9;bytes=208;p32(data,2);local(data+16,b);p32(data+24,3);p32(data+28,1);
        p32(data+40,b->target.basic_rates>>4);p32(data+44,!!(b->target.capabilities&32));
        p32(data+48,!!(b->target.capabilities&1024));
        p32(data+136,b->target.beacon_interval);p32(data+140,b->target.beacon_interval*b->target.dtim_period);break;
    case 5: /* STA_CONFIG v1, station0/link0, legacy non-HT, unassociated. */
        code=10;bytes=96;
        for(unsigned n=0;n<6;n++)data[8+n]=data[16+n]=b->target.bssid[n];break;
    case 6: /* SCD_QUEUE_CONFIG v3: native TFH+byte-count memory, station0/TID15. */
        group=5;code=23;bytes=36;p32(data+4,1);data[8]=15;p32(data+16,5);
        p64(data+20,(uintptr_t)b->management_bc);p64(data+28,(uintptr_t)b->management_tx);break;
    default:return "station setup state";
    }
    o->log(o->opaque,"AX211_STATION_SETUP_STEP",b->prepare_step);
    const char *error=wifi_command(b,o,group,code,version,data,bytes);
    if(!error)++b->prepare_step;
    return error;
}
const char *wifi_prepare_reply(WifiBoot *b,const uint8_t *p,unsigned bytes) {
    if(!b->prepare_step || b->prepare_step>7 || b->prepare_complete)return "station setup reply state";
    if(b->prepare_step==7) {
        if(bytes!=8 || u16(p)!=1 || u16(p+4))
            return "management queue response ID/write pointer";
        /* Both iwx_enable_txq and Linux iwl_pcie_txq_alloc_response ignore
         * this legacy field despite its header comment. Keep it RAW; a real
         * single-frame completion + matching AP reply must prove TX later. */
        b->management_flags=u16(p+2);b->management_aux=u16(p+6);
        b->management_qid=1;b->prepare_complete=1;
    } /* Other context commands use no WANT_RESP in both upstream drivers:
       * transport ACK/failure tags matter, unrequested reply bodies do not. */
    return 0;
}
