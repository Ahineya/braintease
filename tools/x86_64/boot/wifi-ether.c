/* Native Ethernet <-> legacy 802.11/LLC boundary. References: OpenBSD iwx_tx,
 * iwx_rx_mpdu_mq/ccmp_decap and Linux v6.12 rxmq.c (BSD alternative).
 * Live mode adds GTK CCMP/TKIP with firmware integrity + host replay checks.
 * Fragments/A-MSDU are excluded. No unprotected data reaches Ethernet. */
#include "wifi.h"
static unsigned u16(const uint8_t *p){return p[0]|(unsigned)p[1]<<8;}
static unsigned be16(const uint8_t *p){return (unsigned)p[0]<<8|p[1];}
static uint32_t u32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void copy(uint8_t *to,const uint8_t *from,unsigned n){for(unsigned i=0;i<n;i++)to[i]=from[i];}
unsigned wifi_data_encode(const WifiBoot *b,const uint8_t *eth,unsigned bytes,uint8_t *out,unsigned capacity) {
    if(!b->wpa.complete || b->wpa.key_installed!=2 || bytes<14 || bytes>WIFI_ETH_FRAME ||
       capacity<bytes+18 || !wifi_equal(eth+6,b->mac,6))return 0;
    for(unsigned i=0;i<24;i++)out[i]=0;
    out[0]=8;out[1]=0x41; /* ToDS, Protected; firmware inserts CCMP IV/PN/MIC. */
    copy(out+4,b->target.bssid,6);copy(out+10,b->mac,6);copy(out+16,eth,6);
    static const uint8_t snap[6]={0xaa,0xaa,3,0,0,0};copy(out+24,snap,6);
    copy(out+30,eth+12,bytes-12);return bytes+18;
}
unsigned wifi_data_decode(WifiBoot *b,const uint8_t *p,unsigned bytes,uint8_t out[WIFI_ETH_FRAME],unsigned *length) {
    return wifi_data_decode_flags(b,p,bytes,out,length,0);
}
unsigned wifi_data_decode_flags(WifiBoot *b,const uint8_t *p,unsigned bytes,uint8_t out[WIFI_ETH_FRAME],unsigned *length,uint32_t packet_flags) {
    *length=0;
    if(!b->wpa.complete || b->wpa.key_installed!=2 || bytes<64)return 0;
    unsigned len=u16(p),pad=(p[3]&0x20)?2:0;const uint8_t *f=p+64;
    if(len>bytes-64 || len<24)return 0;
    unsigned group=!!(f[4]&1);
    if((f[0]!=8 && f[0]!=0x88) || (f[1]&0xc7)!=0x42 || (u16(f+22)&15) ||
       p[42]!=b->target.channel || (group?!b->live.active:!wifi_equal(f+4,b->mac,6)) ||
       !wifi_equal(f+10,b->target.bssid,6))return 0;
    WifiData *d=&b->data;d->last_status=u32(p+12);
    unsigned header=f[0]==0x88?26:24;
    if(len<header+8+pad+8 || (p[3]&0x40) || (header==26 && (f[24]&0x80)))goto drop;
    /* Modern Linux CCM accepts SEC_CCM + MIC_OK; DECRYPTED/bit11 is retained
     * raw rather than inventing an additional gate absent in that consumer.
     * CRC/overrun are independently checked. Opaque status bits are ignored. */
    if((d->last_status&3)!=3)goto drop;
    unsigned tkip=group && b->target.group_cipher==4;
    unsigned kid=f[header+3]>>6;
    WifiGroupKey *gtk=group?&b->rekey.keys[kid]:0;
    if(group && (!gtk->valid || gtk->bytes!=(tkip?32U:16U)))goto drop;
    if((d->last_status&0x700)!=(tkip?0x300U:0x200U))goto drop;
    if(tkip && !(d->last_status&0x20))goto drop; /* ICV, modern gen2 consumer */
    if(!(d->last_status&0x40)){if(tkip)++d->group_mic_bad;goto drop;}
    unsigned tid=header==26?f[24]&15:0;if(tid>=8)goto drop; /* no HCCA */
    const uint8_t *iv=f+header;
    if(!(iv[3]&0x20) || (!group && kid))goto drop;
    uint64_t pn=(uint64_t)iv[tkip?2:0]|(uint64_t)iv[tkip?0:1]<<8|(uint64_t)iv[4]<<16|
        (uint64_t)iv[5]<<24|(uint64_t)iv[6]<<32|(uint64_t)iv[7]<<40;
    uint64_t *last=group?&gtk->pn[tid]:&d->rx_pn[tid];
    if(pn<=*last){++d->replays;goto drop;}
    /* Both upstream paths skip padding AFTER the crypto IV. */
    const uint8_t *llc=iv+8+pad,*body=llc+8;unsigned available=len-header-8-pad-8;
    if(tkip) {
        /* Linux create_skb removes indicated MIC/CRC trailer bytes first.
         * RADA gen2 strips ICV+Michael, using firmware ICV/MIC results. With
         * RADA off, mac80211 additionally verifies the retained Michael MIC
         * after hardware TKIP decryption and ICV validation. Match both. */
        unsigned trailer=(p[2]>>4)*2;
        if(trailer>available)goto drop;available-=trailer;
        if(!(packet_flags&(1U<<26))) {
            if(available<12)goto drop;uint8_t mic[8];
            wifi_michael(gtk->key+16,f+4,f+16,tid,llc,8+available-12,mic);
            int valid=wifi_equal(mic,body+available-12,8);wifi_wipe(mic,sizeof(mic));
            if(!valid){++d->group_mic_bad;goto drop;}available-=12;
        }
    }
    static const uint8_t snap[6]={0xaa,0xaa,3,0,0,0};if(!wifi_equal(llc,snap,6))goto drop;
    unsigned type=be16(llc+6),payload=0;
    /* Trim using actual protocol length. Some firmware leaves a trailing MIC;
     * it is not payload, and must not turn a full-MTU packet into an oversize
     * Ethernet frame. Do not guess a required MIC/FCS trailer size. */
    if(type==0x806) {
        if(available<28 || be16(body)!=1 || be16(body+2)!=0x800 || body[4]!=6 || body[5]!=4)goto drop;
        payload=28;
    } else if(type==0x800) {
        if(available<20 || (body[0]>>4)!=4 || (body[0]&15)<5)goto drop;
        payload=be16(body+2);if(payload<(body[0]&15)*4U)goto drop;
    } else if(type==0x86dd) {
        if(available<40 || (body[0]>>4)!=6)goto drop;
        payload=40+be16(body+4);
    } else if(type==0x888e && b->live.active) {
        if(available<4)goto drop;payload=4+be16(body+2);
    } else goto drop;
    if(payload>available || payload>1500)goto drop;
    copy(out,f+4,6);copy(out+6,f+16,6);copy(out+12,llc+6,2);copy(out+14,body,payload);
    *last=pn;d->last_pn=pn;++d->rx_packets;*length=14+payload;return 1;
 drop:
    ++d->drops;return 0;
}
