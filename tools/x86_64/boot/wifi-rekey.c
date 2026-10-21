/* Live RSN group-key handshake. Source semantics: OpenBSD pae_input/output,
 * hostap2.11 rsn_supp/wpa and Intel BSD mld-key/datapath (see wifi-LICENSE).
 * Ordinary GTK rotation never resets association, PTK, IP or the TX queue.
 * New pairwise handshakes still require the bounded reconnect fallback. */
#include "wifi.h"
static unsigned be16(const uint8_t *p){return (unsigned)p[0]<<8|p[1];}
static uint64_t be64(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=v<<8|p[i];return v;}
static void copy(uint8_t *a,const uint8_t *b,unsigned n){for(unsigned i=0;i<n;i++)a[i]=b[i];}
static void p16(uint8_t *p,unsigned v){p[0]=v;p[1]=v>>8;}
static void p32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=v>>(8*i);}
static void p64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=v>>(8*i);}
static void count(uint32_t *n){if(*n!=UINT32_MAX)++*n;}
static const char *reject(WifiRekey *r){count(&r->rejected);return 0;}

const char *wifi_rekey_start(WifiBoot *b) {
    WifiRekey *r=&b->rekey;*r=(WifiRekey){0};
    if(b->wpa.gtk_id>3 || (b->wpa.gtk_bytes!=16 && b->wpa.gtk_bytes!=32))return "live GTK state";
    r->command_page=wifi_dma_reserve(b,4096);
    if(!r->command_page)return "live key command DMA allocation";
    WifiGroupKey *k=&r->keys[b->wpa.gtk_id];k->valid=1;k->bytes=b->wpa.gtk_bytes;
    copy(k->key,b->wpa.gtk,k->bytes);
    for(unsigned i=0;i<16;i++)k->pn[i]=b->wpa.gtk_rsc;
    r->replay=b->wpa.m3_replay;return 0;
}
static void response(WifiBoot *b,unsigned kind,uint64_t replay) {
    uint8_t *p=b->rekey.response;wifi_wipe(p,99);
    p[0]=1;p[1]=3;p[2]=0;p[3]=95;p[4]=2;p[5]=3;p[6]=kind==4?10:2;
    for(unsigned i=0;i<8;i++)p[16-i]=replay>>(8*i);
    uint8_t mic[20];wifi_hmac_sha1(b->wpa.ptk,16,p,99,mic);copy(p+81,mic,16);wifi_wipe(mic,sizeof(mic));
}
const char *wifi_rekey_input(WifiBoot *b,const uint8_t *p,unsigned bytes,uint64_t now) {
    WifiRekey *r=&b->rekey;
    if(bytes<99 || p[1]!=3)return reject(r);
    unsigned total=4+be16(p+2),info=be16(p+5),pay=be16(p+97);
    if(p[4]!=2 || total<99 || total>bytes || total>611 || pay>total-99 || pay>512)return reject(r);
    uint64_t replay=be64(p+9);
    if(replay<r->replay)return reject(r);
    if(info==0x8a && replay>r->replay)return "live pairwise rekey requires reconnect";
    /* Both group G1 and a lost initial M4 retry must authenticate with the
     * installed KCK. Malformed/forged radio input cannot reset this link. */
    if(info!=0x1382 && info!=0x13ca)return reject(r);
    uint8_t authenticated[611],mic[20],digest[20];copy(authenticated,p,total);
    wifi_wipe(authenticated+81,16);wifi_hmac_sha1(b->wpa.ptk,16,authenticated,total,mic);
    int valid=wifi_equal(mic,p+81,16);wifi_wipe(authenticated,sizeof(authenticated));wifi_wipe(mic,sizeof(mic));
    if(!valid)return reject(r);
    wifi_sha1(p,total,digest);
    if(replay==r->replay && r->kind) {
        if(!wifi_equal(digest,r->digest,20))return reject(r);
        count(&r->duplicates);
        if(!r->stage && r->retries<4){++r->retries;r->stage=3;r->deadline=now+5000000;}
        return 0; /* Pending duplicate never overwrites DMA or resets PN. */
    }
    if(r->stage)return 0; /* One transaction; AP retransmits newer G1 later. */
    if(info==0x13ca) {
        /* Validate the original M3 against frozen handshake data, responding
         * with M4 only. Never run the initial key-install path a second time. */
        if(replay<r->replay || !wifi_equal(b->wpa.anonce,p+17,32))return reject(r);
        WifiWpa retry=b->wpa;retry.complete=0;retry.m4_count=0;
        const char *error=wifi_wpa_input(&retry,b->mac,&b->target,p,total);
        valid=!error && retry.pending==4;
        wifi_wipe(&retry,sizeof(retry));if(!valid)return reject(r);
        r->kind=4;r->changed=0;
    } else {
        /* RSN senders such as OpenBSD leave key_length zero; actual GTK KDE
         * length is authoritative (unlike the WPA1 group-key format). */
        if(replay<=r->replay)return reject(r);
        uint8_t plain[512],key[32]={0};unsigned expected=b->target.group_cipher==4?32:16;
        if(!wifi_unwrap(b->wpa.ptk+16,p+99,pay,plain)){wifi_wipe(plain,sizeof(plain));wifi_wipe(key,sizeof(key));return reject(r);}
        unsigned at=0,size=pay-8,found=0,kid=0;
        valid=1;
        while(at<size) {
            if(plain[at]==221 && (at+1==size || !plain[at+1])) {
                for(unsigned i=at+1;i<size;i++)if(plain[i])valid=0;
                break;
            }
            if(size-at<2 || plain[at+1]>size-at-2){valid=0;break;}
            unsigned n=plain[at+1];
            if(plain[at]==221 && n>=4 && plain[at+2]==0 && plain[at+3]==15 && plain[at+4]==172 && plain[at+5]==1) {
                if(found++ || n!=6+expected){valid=0;break;}
                kid=plain[at+6]&3;copy(key,plain+at+8,expected);
            }
            /* PMF/IGTK and changed security selection are not negotiated. */
            if(plain[at]==48){valid=0;break;}
            at+=n+2;
        }
        if(!found)valid=0;
        WifiGroupKey *slot=&r->keys[kid];
        /* Do not reinstall an existing key, even with a fresh replay value.
         * Refuse moving the same material between IDs rather than reset PN. */
        for(unsigned i=0;i<4;i++)if(i!=kid && r->keys[i].valid && r->keys[i].bytes==expected && wifi_equal(r->keys[i].key,key,expected))valid=0;
        if(valid) {
            r->kind=2;r->key_id=kid;r->key_bytes=expected;
            r->changed=!slot->valid || slot->bytes!=expected || !wifi_equal(slot->key,key,expected);
            if(r->changed)copy(r->key,key,expected);else wifi_wipe(r->key,sizeof(r->key));
            r->rsc=0;
            for(unsigned i=0;i<6;i++)r->rsc|=(uint64_t)p[65+i]<<(8*i);
        }
        wifi_wipe(plain,sizeof(plain));wifi_wipe(key,sizeof(key));
        if(!valid)return reject(r);
    }
    r->replay=replay;copy(r->digest,digest,20);r->retries=0;
    response(b,r->kind,replay);r->stage=r->changed?1:3;r->deadline=now+5000000;
    return 0;
}
static const char *install(WifiBoot *b,const WifiOps *o) {
    WifiRekey *r=&b->rekey;
    if(b->command_pending)return "live key command already owned";
    unsigned index=b->command_count&255;uint8_t *cmd=r->command_page,*key=cmd+8,*desc=b->tx+index*256;
    wifi_wipe(cmd,4096);wifi_wipe(desc,256);
    cmd[0]=24;cmd[1]=5;cmd[2]=index;p16(cmd+4,80);
    p32(key,1);p32(key+4,1);p32(key+8,r->key_id);p32(key+12,b->target.group_cipher==4?0x43:0x42);
    if(r->key_bytes==16)copy(key+16,r->key,16);
    else {
        copy(key+16,r->key,16);copy(key+32,r->key+24,8);copy(key+40,r->key+16,8);
        copy(key+48,r->key+16,8);copy(key+56,r->key+24,8);
    }
    /* Like upstream SEC_KEY ADD, RX replay validation is host-owned, per TID;
     * firmware rx_seq/tx_seq remain zero. Never touch the PTK command. */
    p16(desc,2);p16(desc+2,20);p64(desc+4,(uintptr_t)cmd);
    p16(desc+12,68);p64(desc+14,(uintptr_t)cmd+20);
    r->command_index=index;r->stage=2;b->command_pending=1;
    b->command_group=5;b->command_code=24;b->command_data=cmd;
    b->command_count=(b->command_count+1)&65535;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);o->write(o->opaque,0x460,b->command_count);
    return 0;
}
const char *wifi_rekey_reply(WifiBoot *b,const uint8_t *p,unsigned bytes) {
    WifiRekey *r=&b->rekey;
    if(bytes<4 || r->stage!=2 || !b->command_pending || p[0]!=24 || p[1]!=5 || p[2]!=r->command_index || p[3]!=0)
        return "live GTK firmware reply tag/flags/state";
    /* SEC_KEY reply payload is opaque in both upstream consumers. */
    WifiGroupKey *k=&r->keys[r->key_id];wifi_wipe(k,sizeof(*k));
    k->valid=1;k->bytes=r->key_bytes;copy(k->key,r->key,k->bytes);
    for(unsigned i=0;i<16;i++)k->pn[i]=r->rsc;
    wifi_wipe(r->key,sizeof(r->key));wifi_wipe(r->command_page,4096);
    b->command_pending=0;r->stage=3;return 0;
}
const char *wifi_rekey_poll(WifiBoot *b,const WifiOps *o,uint64_t now) {
    WifiRekey *r=&b->rekey;if(!r->stage)return 0;
    if(now>=r->deadline)return "live group key refresh timeout";
    if(r->stage==1)return install(b,o);
    if(r->stage==3) {
        uint8_t eth[113];copy(eth,b->target.bssid,6);copy(eth+6,b->mac,6);eth[12]=0x88;eth[13]=0x8e;copy(eth+14,r->response,99);
        unsigned result=wifi_live_control_tx(b,o,eth,sizeof(eth),now);
        if(result==NIC_OK){r->tx_wait=b->live.tx_head;r->stage=4;}
        else if(result!=NIC_WOULD_BLOCK)return "live key response TX unavailable";
    }
    return 0;
}
const char *wifi_rekey_tx_done(WifiBoot *b,const WifiOps *o,unsigned end,unsigned status) {
    WifiRekey *r=&b->rekey;
    if(r->stage!=4 || end!=r->tx_wait)return 0;
    /* Radio retries can fail normally. Resend the already-authenticated G2,
     * without installing a key again, within the original time/retry budget. */
    if(status!=1 && status!=2) {
        if(++r->retries>4)return "live key response retry limit";
        r->stage=3;return 0;
    }
    if(r->changed){count(&r->rotations);r->changed=0;o->log(o->opaque,"AX211_GROUP_REKEY_OK",r->rotations);}
    r->stage=0;return 0;
}
