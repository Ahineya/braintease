/* One protected ARP probe after the accepted WPA2 key exchange. No claimed IP,
 * DHCP, persistent NIC or group traffic. Same native Gen3 queue, fresh page,
 * no ring reuse. Source semantics and notices in wifi-LICENSE.txt/audit. */
#include "wifi.h"
static unsigned u16(const uint8_t *p){return p[0]|(unsigned)p[1]<<8;}
static uint32_t u32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void p16(uint8_t *p,unsigned v){p[0]=v;p[1]=v>>8;}
static void p32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=v>>(8*i);}
static void p64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=v>>(8*i);}
const char *wifi_data_advance(WifiBoot *b,const WifiImage *fw,const WifiOps *o) {
    WifiData *d=&b->data;
    if(!fw->data_probe || !b->wpa.complete || b->command_pending || d->complete)return 0;
    if(d->sent){if(!d->tx_wait && d->reply)d->complete=1;return 0;}
    if(b->wpa.tx_wait || b->wpa.tx_next>=256)return "data TX ownership";
    unsigned rate=0;while(rate<8 && !(b->target.basic_rates&(1U<<(rate+4))))++rate;
    if(rate==8)return "data missing basic OFDM rate";
    uint8_t *cmd=wifi_dma_reserve(b,4096);if(!cmd)return "data TX DMA allocation";
    uint8_t eth[42]={0},frame[60];
    for(unsigned i=0;i<6;i++){eth[i]=255;eth[6+i]=eth[22+i]=b->mac[i];}
    eth[12]=8;eth[13]=6;eth[15]=1;eth[16]=8;eth[18]=6;eth[19]=4;eth[21]=1;
    for(unsigned i=0;i<4;i++)eth[38+i]=d->target[i]; /* SPA0, no address claim */
    unsigned len=wifi_data_encode(b,eth,sizeof(eth),frame,sizeof(frame));
    if(len!=60)return "data Ethernet encapsulation";
    unsigned index=b->wpa.tx_next;cmd[0]=0x1c;cmd[2]=index;cmd[3]=1;
    p16(cmd+4,len);p16(cmd+6,1); /* CMD_RATE, encryption ENABLED */
    p32(cmd+8,12U<<8);p32(cmd+20,(1U<<14)|(1U<<8)|rate);
    for(unsigned i=0;i<24;i++)cmd[32+i]=frame[i];p16(cmd+54,index<<4);
    for(unsigned i=0;i<36;i++)cmd[64+i]=frame[24+i];
    uint8_t *desc=b->management_tx+index*256;
    p16(desc,3);p16(desc+2,20);p64(desc+4,(uintptr_t)cmd);
    p16(desc+12,36);p64(desc+14,(uintptr_t)cmd+20);
    p16(desc+22,36);p64(desc+24,(uintptr_t)cmd+64);p16(b->management_bc+index*2,len);
    ++b->wpa.tx_next;d->sent=1;d->tx_wait=b->wpa.tx_next;++b->nvm_progress;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);o->write(o->opaque,0x460,(1U<<16)|d->tx_wait);
    o->log(o->opaque,"AX211_DATA_ARP_SUBMITTED",d->tx_wait);return 0;
}
const char *wifi_data_tx_reply(WifiBoot *b,const uint8_t *p,unsigned bytes) {
    WifiData *d=&b->data;
    if(!d->tx_wait || bytes<52)return "data TX completion size/state";
    const uint8_t *tx=p+4;
    if(tx[0]!=1 || u16(tx+36)!=1 || (u32(tx+44)&65535)!=d->tx_wait)return "data TX completion count/queue/SSN";
    d->tx_status=u16(tx+40)&255;
    if(d->tx_status!=1 && d->tx_status!=2)return "protected data TX failed";
    d->tx_wait=0;++b->nvm_progress;return 0;
}
const char *wifi_data_mpdu(WifiBoot *b,const uint8_t *p,unsigned bytes) {
    if(!b->data.enabled || !b->data.sent || b->data.complete)return 0;
    if(bytes<64 || u16(p)>bytes-64)return "data RX descriptor bounds";
    uint8_t eth[WIFI_ETH_FRAME];unsigned len=0;
    if(!wifi_data_decode(b,p,bytes,eth,&len) || len!=42 || eth[12]!=8 || eth[13]!=6)return 0;
    const uint8_t *arp=eth+14;
    if(arp[6]!=0 || arp[7]!=2 || !wifi_mac_valid(eth+6) || !wifi_equal(arp+8,eth+6,6) ||
       !wifi_equal(arp+14,b->data.target,4) || !wifi_equal(arp+18,b->mac,6))return 0;
    for(unsigned i=0;i<4;i++)if(arp[24+i])return 0;
    for(unsigned i=0;i<6;i++)b->data.peer[i]=eth[6+i];
    if(!b->data.reply){b->data.reply=1;++b->nvm_progress;}return 0;
}
