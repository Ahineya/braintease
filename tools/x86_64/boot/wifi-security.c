/* Bounded 802.11 beacon information elements, native octets only.
 * RSN suite selectors use IEEE OUI00:0f:ac. No WPA1/open downgrade or keys. */
#include "wifi.h"
static unsigned u16(const uint8_t *p){return p[0]|(unsigned)p[1]<<8;}
static unsigned suite(const uint8_t *p) {
    return p[0]==0 && p[1]==15 && p[2]==172 && p[3]<32?1U<<p[3]:0;
}
static const char *rsn(WifiAp *ap,const uint8_t *p,unsigned bytes) {
    if(bytes<18 || u16(p)!=1)return "RSN version/size";
    ap->group_cipher=suite(p+2);
    ap->group_suite=p[2]|(uint32_t)p[3]<<8|(uint32_t)p[4]<<16|(uint32_t)p[5]<<24;
    unsigned count=u16(p+6),at=8;
    if(!count || count>(bytes-at)/4)return "RSN pairwise count";
    for(unsigned n=0;n<count;n++,at+=4)ap->pair_ciphers|=suite(p+at);
    if(bytes-at<2)return "RSN missing AKM count";
    count=u16(p+at);at+=2;
    if(!count || count>(bytes-at)/4)return "RSN AKM count";
    for(unsigned n=0;n<count;n++,at+=4)ap->akms|=suite(p+at);
    if(at==bytes)return 0; /* RSN capabilities are optional. */
    if(bytes-at<2)return "RSN capabilities bounds";
    ap->rsn_caps=u16(p+at);at+=2;
    if(at==bytes)return 0;
    if(bytes-at<2)return "RSN PMKID count bounds";
    count=u16(p+at);at+=2;
    if(count>(bytes-at)/16)return "RSN PMKID bounds";
    at+=16*count;
    if(at==bytes)return 0;
    if(bytes-at!=4 || !suite(p+at))return "RSN group management/trailing bytes";
    return 0;
}
const char *wifi_ap_details(WifiAp *ap,const uint8_t *p,unsigned bytes) {
    static const uint8_t legacy[12]={2,4,11,22,12,18,24,36,48,72,96,108};
    ap->rsn_size=0;
    ap->details_valid=ap->rates=ap->basic_rates=ap->unknown_basic=ap->rsn_caps=0;
    ap->group_cipher=ap->pair_ciphers=ap->akms=ap->group_suite=ap->dtim_period=0;
    unsigned seen_rsn=0,seen_rates=0,seen_extended=0,seen_tim=0;
    for(unsigned at=0;at<bytes;) {
        if(bytes-at<2 || p[at+1]>bytes-at-2)return "beacon IE bounds";
        unsigned id=p[at],n=p[at+1];at+=2;
        if(id==48) {
            if(seen_rsn++)return "duplicate RSN IE";
            const char *error=rsn(ap,p+at,n);if(error)return error;
            ap->rsn_size=n+2;for(unsigned j=0;j<n+2;j++)ap->rsn_ie[j]=p[at-2+j];
        }
        if(id==1 || id==50) {
            if(!n || (id==1 && (n>8 || seen_rates++)) ||
               (id==50 && seen_extended++))return "supported rates bounds/duplicate";
            for(unsigned j=0;j<n;j++) {
                unsigned r=p[at+j]&127,index=0;
                for(;index<12 && legacy[index]!=r;index++){}
                if(index<12) {
                    ap->rates|=1U<<index;
                    if(p[at+j]&128)ap->basic_rates|=1U<<index;
                } else if(p[at+j]&128)ap->unknown_basic=1;
            }
        }
        if(id==5) {
            if(n<4 || !p[at+1] || seen_tim++)return "TIM bounds/period/duplicate";
            ap->dtim_period=p[at+1];
        }
        at+=n;
    }
    if(!seen_rates)return "missing supported rates IE";
    ap->details_valid=1;return 0;
}
/* This is preparation-only eligibility, NOT a secured-data capability test.
 * A WPA2 BSS may use CCMP pairwise with TKIP group traffic. Neither group
 * cipher is installed/used by this no-transmit firmware-context checkpoint. */
static const char *setup_compatible(const WifiAp *ap) {
    if(!(ap->capabilities&1) || (ap->capabilities&2) || !ap->beacon_interval)
        return "target is not an infrastructure BSS";
    if(!ap->privacy || !ap->rsn)return "target requires WPA2 RSN; no open/WPA1 fallback";
    if(!ap->details_valid)return "target beacon security/rate details malformed";
    if(ap->group_cipher!=(1U<<4) && ap->group_cipher!=(1U<<2))
        return "station setup supports CCMP or TKIP group advertisement only";
    if(!(ap->pair_ciphers&(1U<<4)))return "target has no CCMP pairwise cipher";
    if(!(ap->akms&(1U<<2)))return "target has no WPA2 PSK authentication";
    if(ap->rsn_caps&(1U<<6))return "target requires protected management frames";
    if(ap->unknown_basic || !ap->basic_rates || !(ap->rates&(1U<<4)) ||
       (ap->channel>14 && (ap->basic_rates&15)))
        return "target requires unsupported basic rates";
    return 0;
}
const char *wifi_target_select(WifiBoot *b,const WifiImage *fw) {
    if(!fw->prepare_ssid_size || fw->prepare_ssid_size>32 || !b->scan_complete ||
       !b->scan_acked || b->command_pending || b->reg_generation!=b->reg_applied_generation)
        return "target selection before stable scan/profile";
    b->target=(WifiAp){0};
    const WifiAp *best=0;const char *error="target SSID not found in completed scan";
    for(unsigned n=0;n<b->scan_count;n++) {
        const WifiAp *ap=&b->aps[n];
        if(ap->ssid_size!=fw->prepare_ssid_size)continue;
        unsigned same=1;
        for(unsigned j=0;j<ap->ssid_size;j++)if(ap->ssid[j]!=fw->prepare_ssid[j])same=0;
        if(!same)continue;
        if(!best)b->target=*ap; /* retain rejected target details for diagnosis */
        const char *why=setup_compatible(ap);if(why){error=why;continue;}
        if(!best || (ap->energy && (!best->energy || ap->energy<best->energy)))best=ap;
    }
    if(!best)return error;
    b->target=*best;return 0;
}

/* The exact selected RSN IE is shared by association and EAPOL message2. */
unsigned wifi_rsn_request(const WifiAp *ap,uint8_t *out) {
    static const uint8_t ie[22]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
    for(unsigned n=0;n<22;n++)out[n]=ie[n];
    out[7]=ap->group_cipher==4?2:4;return 22;
}
