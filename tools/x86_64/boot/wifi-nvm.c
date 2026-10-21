/* Three read-only initialization commands, following OpenBSD iwx.
 * See wifi-LICENSE.txt. No scan, association or data TX commands. */
#include "wifi.h"
static uint32_t u32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static unsigned u16(const uint8_t *p){return p[0]|(unsigned)p[1]<<8;}
static void p16(uint8_t *p,unsigned v){p[0]=v;p[1]=v>>8;}
static void p32(uint8_t *p,uint32_t v){for(unsigned n=0;n<4;n++)p[n]=v>>(8*n);}
static void p64(uint8_t *p,uint64_t v){for(unsigned n=0;n<8;n++)p[n]=v>>(8*n);}
const char *wifi_nvm_parse(WifiBoot *b,const uint8_t *p,unsigned bytes,unsigned version) {
    if((version!=3 && version!=4) || bytes!=(version==4?468U:128U))return "NVM reply size/version";
    unsigned channels=version==4?u32(p+24):51;
    if(!channels || channels>110)return "NVM channel count";
    unsigned nvm_version=u16(p+4),tx=u32(p+12),rx=u32(p+16),flags=u32(p+8);
    /* iwl_get_nvm logs empty OTP/zero address count and retains the NVM
     * version. They do not invalidate a separately verified CSR MAC. */
    b->nvm_general_flags=u32(p);b->nvm_lar=u32(p+20);
    if(!(flags&3))return "NVM no enabled bands";
    if(!tx || (tx&~3U))return "NVM TX antenna mask";
    if(!rx || (rx&~3U))return "NVM RX antenna mask";
    /* LAR is tested as nonzero by both iwx and iwlwifi, not restricted to1. */
    unsigned valid=0;
    for(unsigned n=0;n<channels;n++) {
        const uint8_t *record=p+(version==4?28:24)+n*(version==4?4:2);
        if((version==4?u32(record):u16(record))&1)++valid;
    }
    b->nvm_version=nvm_version;b->nvm_channels=channels;b->nvm_valid_channels=valid;
    b->nvm_tx_chains=tx;b->nvm_rx_chains=rx;b->nvm_mac_flags=flags;b->nvm_addresses=p[7];
    for(unsigned n=0;n<18 && n<channels;n++)
        b->nvm_channel_flags[n]=version==4?u32(p+28+4*n):u16(p+24+2*n);
    b->nvm_complete=1;
    return 0;
}
const char *wifi_nvm_packet(WifiBoot *b,const WifiImage *fw,const uint8_t *p,unsigned bytes) {
    if(bytes<8)return "NVM RX header";
    unsigned size=u32(p)&0x3fff;
    if(size<4 || size>bytes-4)return "NVM RX bounds";
    if(!fw->nvm_probe || !b->pnvm_complete)return 0;
    if(fw->auth_probe && p[4]==0x1c && p[5]==0)return b->data.sent?wifi_data_tx_reply(b,p+4,size):b->wpa.tx_next>2?wifi_wpa_tx_reply(b,p+4,size):b->assoc_sent?wifi_assoc_tx_reply(b,p+4,size):wifi_auth_tx_reply(b,p+4,size);
    if(p[4]==2 && p[5]==0)return "firmware command error reply";
    if(p[4]==4 && p[5]==0) {
        if(!(p[7]&0x80) || !b->command_count || b->init_complete)return "INIT notification state";
        b->init_complete=1;++b->nvm_progress;return 0;
    }
    if(p[7]&0x80)return fw->scan_probe?wifi_scan_notification(b,p+4,size):0; /* Unrelated firmware-origin notification. */
    if(!b->command_pending || p[7]!=0 || p[6]!=b->command_count-1 ||
       p[4]!=b->command_code || (p[5]&~0x40U)!=b->command_group)return "command reply tag/state";
    if(p[5]&0x40)return "firmware command failed flag";
    if(b->command_count>3) {
        const char *error=wifi_scan_reply(b,p+8,size-4);
        if(error)return error;
    }
    if(b->command_count==3) {
        if(!b->init_complete)return "NVM reply before init complete";
        const char *error=wifi_nvm_parse(b,p+8,size-4,fw->nvm_notification);
        if(error)return error;
    }
    b->command_pending=0;++b->nvm_progress;return 0;
}
const char *wifi_nvm_advance(WifiBoot *b,const WifiImage *fw,const WifiOps *o) {
    if(!fw->nvm_probe || !b->pnvm_complete || b->nvm_complete || b->command_pending)return 0;
    if(b->command_count==2 && !b->init_complete)return 0;
    if(b->command_count>=3)return "command queue exhausted";
    if(!b->command_data) {
        b->command_data=wifi_dma_reserve(b,4096);
        if(!b->command_data)return "command DMA allocation";
    }
    unsigned index=b->command_count;
    uint8_t *cmd=b->command_data+index*64,*desc=b->tx+index*256;
    unsigned group=index==0?2:12,code=index==0?3:index==1?0:2;
    cmd[0]=code;cmd[1]=group;cmd[2]=index;cmd[3]=0;
    p16(cmd+4,4);cmd[6]=cmd[7]=0;p32(cmd+8,index==0?2:0); /* INIT_NVM bit1 */
    p16(desc,1);p16(desc+2,12);p64(desc+4,(uintptr_t)cmd); /* one TFH TB, <=20 bytes */
    b->command_code=code;b->command_group=group;b->command_pending=1;
    ++b->command_count;++b->nvm_progress;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    o->write(o->opaque,0x460,b->command_count); /* queue0, native producer */
    o->log(o->opaque,"AX211_NVM_COMMAND_SUBMITTED",group<<8|code);
    return 0;
}
