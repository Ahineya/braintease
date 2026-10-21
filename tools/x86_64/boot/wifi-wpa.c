/* Native bounded supplicant. Semantics: OpenBSD pae_input1.39/output1.33,
 * crypto1.81 (pinned SHA in wifi-crypto-sources.json/reference manifest).
 * See wifi-LICENSE.txt. PSK/CCMP pairwise, CCMP or TKIP GTK; no WPA1/SAE/PMF.
 * Authenticated M3 replay responses never reset/reinstall temporal keys. */
#include "wifi.h"
static unsigned be16(const uint8_t *p){return (unsigned)p[0]<<8|p[1];}
static uint64_t be64(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=v<<8|p[i];return v;}
static void put16(uint8_t *p,unsigned v){p[0]=v>>8;p[1]=v;}
static void put64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[7-i]=v>>(8*i);}
static void copy(uint8_t *a,const uint8_t *b,unsigned n){for(unsigned i=0;i<n;i++)a[i]=b[i];}
void wifi_wpa_init(WifiWpa *w,const uint8_t pmk[32],const uint8_t snonce[32]) {
    *w=(WifiWpa){.enabled=1,.tx_next=2};copy(w->pmk,pmk,32);copy(w->snonce,snonce,32);
}
static void response(WifiWpa *w,const WifiAp *ap,unsigned kind,uint64_t replay) {
    wifi_wipe(w->pending_data,sizeof(w->pending_data));uint8_t *p=w->pending_data;
    p[0]=1;p[1]=3;p[4]=2;put16(p+5,kind==2?0x10a:0x30a);put64(p+9,replay);
    unsigned bytes=99;
    if(kind==2){copy(p+17,w->snonce,32);bytes+=wifi_rsn_request(ap,p+99);}
    put16(p+2,bytes-4);put16(p+97,bytes-99);
    uint8_t mic[20];wifi_hmac_sha1(w->ptk,16,p,bytes,mic);copy(p+81,mic,16);wifi_wipe(mic,sizeof(mic));
    w->pending=kind;w->pending_bytes=bytes;
}
static const char *message1(WifiWpa *w,const uint8_t local[6],const WifiAp *ap,const uint8_t *p,unsigned bytes) {
    unsigned info=be16(p+5);uint64_t replay=be64(p+9);
    if(info&(0x100|0x400|0x800|0x1000|0x2000))return "unsupported WPA2 message1 flags";
    if(w->m3_seen)return 0; /* bounded probe never begins a second handshake */
    if(w->m1_seen && replay<w->m1_replay)return 0;
    for(unsigned at=99;at<bytes;) {
        if(bytes-at<2 || p[at+1]>bytes-at-2)return "WPA2 message1 IE bounds";
        if(p[at]==221 && p[at+1]>=4 && p[at+2]==0 && p[at+3]==15 && p[at+4]==172 &&
           p[at+5]==4 && p[at+1]!=20)return "WPA2 PMKID length";
        at+=2+p[at+1];
    }
    if(w->m1_seen && !wifi_equal(w->anonce,p+17,32))return "WPA2 ANonce changed during initial handshake";
    if(w->m2_count>=4)return "WPA2 message1 retry limit";
    if(!w->m1_seen){copy(w->anonce,p+17,32);wifi_ptk(w->pmk,ap->bssid,local,w->anonce,w->snonce,w->ptk);}
    else ++w->duplicates;
    w->m1_seen=1;w->m1_replay=replay;response(w,ap,2,replay);return 0;
}
static const char *message3(WifiWpa *w,const WifiAp *ap,const uint8_t *p,unsigned bytes) {
    if(!w->m1_seen || !w->m2_count)return "WPA2 message3 before message2 submission";
    uint64_t replay=be64(p+9);unsigned info=be16(p+5),wrapped=be16(p+97);
    if(replay<w->m1_replay || (w->m3_seen && replay<w->m3_replay))return 0;
    if((info&0x13c8)!=0x13c8 || (info&(0x400|0x800|0x2000)) || be16(p+7)!=16)
        return "unsupported WPA2 message3 flags/key length";
    if(!wifi_equal(w->anonce,p+17,32))return "WPA2 message3 ANonce mismatch";
    uint8_t authenticated[611],mic[20],plain[512];copy(authenticated,p,bytes);
    wifi_wipe(authenticated+81,16);wifi_hmac_sha1(w->ptk,16,authenticated,bytes,mic);
    int valid=wifi_equal(mic,p+81,16);wifi_wipe(authenticated,sizeof(authenticated));wifi_wipe(mic,sizeof(mic));
    if(!valid)return "WPA2 message3 MIC mismatch";
    if(!wifi_unwrap(w->ptk+16,p+99,wrapped,plain))return "WPA2 key-data wrap integrity";
    const char *error=0;unsigned at=0,rsns=0,gtk_at=0,gtk_n=0;uint8_t gtk[32]={0};
    unsigned size=wrapped-8;
    while(at<size) {
        /* RFC3394 WPA padding is DD followed by zero octets, including DD00. */
        if(plain[at]==221 && (at+1==size || plain[at+1]==0)) {
            for(unsigned i=at+1;i<size;i++)if(plain[i]){error="WPA2 key-data padding";goto done;}break;
        }
        if(size-at<2 || plain[at+1]>size-at-2){error="WPA2 key-data IE bounds";goto done;}
        unsigned id=plain[at],n=plain[at+1];
        if(id==48) {
            if(!rsns++) {
                if(ap->rsn_size!=n+2 || !wifi_equal(ap->rsn_ie,plain+at,n+2)) {error="WPA2 AP RSN changed since beacon";goto done;}
            } else if(rsns==2) {
                uint8_t ies[260]={1,1,0x8c};copy(ies+3,plain+at,n+2);WifiAp second={0};
                error=wifi_ap_details(&second,ies,n+5);
                if(error || second.group_cipher!=ap->group_cipher || second.pair_ciphers!=16 || second.akms!=4 || (second.rsn_caps&64)) {error="WPA2 unsupported second RSN selection";goto done;}
            }
        }
        if(id==221 && n>=4 && plain[at+2]==0 && plain[at+3]==15 && plain[at+4]==172 && plain[at+5]==1) {
            unsigned expected=ap->group_cipher==4?32:16;
            if(gtk_n || n!=6+expected){error="WPA2 GTK size/duplicate";goto done;}
            gtk_at=at;gtk_n=expected;copy(gtk,plain+at+8,expected);
        }
        at+=n+2;
    }
    if(!rsns || !gtk_n){error="WPA2 message3 missing AP RSN/GTK";goto done;}
    unsigned kid=plain[gtk_at+6]&3;
    if(w->m3_seen) {
        if(w->gtk_bytes!=gtk_n || w->gtk_id!=kid || !wifi_equal(w->gtk,gtk,gtk_n)) {error="WPA2 GTK changed in message3 retry";goto done;}
        if(w->m4_count>=4){error="WPA2 message3 retry limit";goto done;}
        ++w->duplicates; /* never reinstall or reset replay counters */
    } else {
        copy(w->gtk,gtk,gtk_n);w->gtk_bytes=gtk_n;w->gtk_id=kid;
        for(unsigned i=0;i<6;i++)w->gtk_rsc|=(uint64_t)p[65+i]<<(8*i);
    }
    w->m3_seen=1;w->m3_replay=replay;response(w,ap,4,replay);
 done:
    wifi_wipe(plain,sizeof(plain));wifi_wipe(gtk,sizeof(gtk));return error;
}
const char *wifi_wpa_input(WifiWpa *w,const uint8_t local[6],const WifiAp *ap,const uint8_t *p,unsigned bytes) {
    if(!w->enabled || w->complete)return 0;
    if(bytes<99)return "WPA2 EAPOL fixed bounds";
    unsigned total=4+be16(p+2),pay=be16(p+97),info=be16(p+5);
    w->last_info=info;w->last_bytes=total;
    if(p[1]!=3)return 0; /* no EAP/802.1X enterprise fallback */
    if(p[4]!=2 || (info&7)!=2)return "unsupported WPA2 descriptor/version";
    if(total<99 || total>bytes || total>611 || pay>total-99 || pay>512)return "WPA2 EAPOL/key-data bounds";
    if(!(info&8) || !(info&128))return "unsupported initial WPA2 key message";
    /* EAPOL padding after the declared key data does not become a new IE. */
    if(info&256)return message3(w,ap,p,total);
    return message1(w,local,ap,p,99+pay);
}
