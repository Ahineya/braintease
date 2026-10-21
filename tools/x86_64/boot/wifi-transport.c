/* Startup subset of OpenBSD iwx, AX210-family only; see wifi-LICENSE.txt.
 * Polled ALIVE/PNVM/init, passive scan and bounded auth/association/EAPOL.
 * The optional key-install checkpoint stops before protected traffic or IP.
 * Ops isolate MMIO/PCI from the same production state machine's host tests. */
#include "wifi.h"
#define HW_IF 0x000
#define INT 0x008
#define INT_MASK 0x00c
#define FH_INT 0x010
#define RESET 0x020
#define GP 0x024
#define LINK_PWR 0x250
static uint32_t read_reg(const WifiOps *o,unsigned r){return o->read(o->opaque,r);}
static void write_reg(const WifiOps *o,unsigned r,uint32_t v){o->write(o->opaque,r,v);}
static void bits(const WifiOps *o,unsigned r,uint32_t set,uint32_t clear) {
    write_reg(o,r,(read_reg(o,r)&~clear)|set);
}
static int poll(const WifiOps *o,unsigned r,uint32_t mask,uint32_t want,unsigned us) {
    for(unsigned t=0;t<=us;t+=10) {
        uint32_t v=read_reg(o,r);
        if(v==UINT32_MAX) return 0;
        if((v&mask)==want) return 1;
        o->delay(o->opaque,10);
    }
    return 0;
}
static void report(const WifiOps *o,const char *s,uint64_t v){o->log(o->opaque,s,v);}
static void barrier(void){__atomic_thread_fence(__ATOMIC_SEQ_CST);}
static int lock(const WifiOps *o) {
    bits(o,GP,8,0); o->delay(o->opaque,2);
    if(poll(o,GP,0x11,1,150000)) return 1;
    bits(o,GP,0,8); return 0;
}
static void unlock(const WifiOps *o){bits(o,GP,0,8);}
static void umac(const WifiOps *o,unsigned address,uint32_t value) {
    write_reg(o,0x444,((address+0x300000)&0xffffff)|0x3000000);
    barrier(); write_reg(o,0x44c,value); barrier();
}
static void interrupt_off(const WifiOps *o) {
    write_reg(o,INT_MASK,0); write_reg(o,INT,UINT32_MAX); write_reg(o,FH_INT,UINT32_MAX);
}
static void reset(const WifiOps *o) {
    bits(o,RESET,0x80,0); o->delay(o->opaque,5000);
}
static int ready(const WifiOps *o) {
    bits(o,HW_IF,0x400000,0);
    if(!poll(o,HW_IF,0x400000,0x400000,50))return 0;
    bits(o,0x88,0x20,0); return 1;
}
static int prepare(const WifiOps *o) {
    if(ready(o))return 1;
    bits(o,LINK_PWR,0x80000000,0); o->delay(o->opaque,1000);
    for(unsigned n=0;n<10;n++) {
        bits(o,HW_IF,0x08000000,0);
        for(unsigned t=0;t<150000;t+=250) {
            if(ready(o))return 1;
            o->delay(o->opaque,200);
        }
        o->delay(o->opaque,25000);
    }
    return 0;
}
static int apm(const WifiOps *o) {
    bits(o,0x100,0x00800000,0); bits(o,0x240,0xffff0000,0);
    bits(o,HW_IF,0x00080000,0); bits(o,0x3c,2,0); bits(o,GP,4,0);
    return poll(o,GP,1,1,25000);
}
static uint64_t address(const uint8_t *p){return (uintptr_t)p;}
static void address_reg(const WifiOps *o,unsigned reg,const uint8_t *p) {
    uint64_t a=address(p);write_reg(o,reg,a);write_reg(o,reg+4,a>>32);
}
static unsigned dma16(const uint8_t *p) {
    /* Device writes aligned native LE u16, acquire barrier after producer load. */
    unsigned value=*(const volatile uint16_t *)(const void *)p;
    barrier(); return value;
}
static void diagnostics(WifiBoot *b,const WifiOps *o) {
    report(o,"AX211_CSR_INT",read_reg(o,INT)); report(o,"AX211_GP",read_reg(o,GP));
    report(o,"AX211_IML_RESP",read_reg(o,0x12c));
    report(o,"AX211_RX_PRODUCER",dma16(b->status));
    report(o,"AX211_COMMAND_COUNT",b->command_count);report(o,"AX211_COMMAND_PENDING",b->command_pending);
    report(o,"AX211_INIT_COMPLETE",b->init_complete);
    report(o,"AX211_RADIO_STEP",b->radio_step);report(o,"AX211_SCAN_ACK",b->scan_acked);
    report(o,"AX211_SCAN_COMPLETE",b->scan_complete);report(o,"AX211_SCAN_RX_FRAMES",b->scan_frames);
    report(o,"AX211_REG_NOTIFICATIONS",b->reg_notifications);report(o,"AX211_REG_REFRESHES",b->reg_refreshes);
    report(o,"AX211_STATION_SETUP_STEP",b->prepare_step);report(o,"AX211_STATION_SETUP_COMPLETE",b->prepare_complete);
    report(o,"AX211_QUEUE_FLAGS_RAW",b->management_flags);
    report(o,"AX211_AUTH_PHASE",b->auth_phase);report(o,"AX211_AUTH_SESSION",b->auth_session);
    report(o,"AX211_AUTH_TX_STATUS",b->auth_tx_status);report(o,"AX211_AUTH_AP_STATUS",b->auth_status);
    report(o,"AX211_ASSOC_TX_STATUS",b->assoc_tx_status);report(o,"AX211_ASSOC_AP_STATUS",b->assoc_status);
    report(o,"AX211_ASSOC_AID",b->assoc_aid);report(o,"AX211_ASSOC_UPDATE_STEP",b->assoc_step);
    report(o,"AX211_WPA2_M1",b->wpa.m1_seen);report(o,"AX211_WPA2_M2",b->wpa.m2_count);
    report(o,"AX211_WPA2_M3",b->wpa.m3_seen);report(o,"AX211_WPA2_M4",b->wpa.m4_count);
    report(o,"AX211_WPA2_KEYS",b->wpa.key_installed);report(o,"AX211_WPA2_TX_STATUS",b->wpa.tx_status);
    if(b->data.enabled) {
        report(o,"AX211_DATA_TX",b->data.tx_status);report(o,"AX211_DATA_RX",b->data.rx_packets);
        report(o,"AX211_DATA_STATUS",b->data.last_status);report(o,"AX211_DATA_REPLAYS",b->data.replays);
        report(o,"AX211_DATA_DROPS",b->data.drops);report(o,"AX211_DATA_ARP_REPLY",b->data.reply);
    }
    barrier();
    report(o,"AX211_BOOT_STAGE",*(volatile uint32_t *)(void *)b->info);
    report(o,"AX211_IPC_STATUS",*(volatile uint32_t *)(void *)(b->info+4));
    report(o,"AX211_LAST_REPLY_HEADER",b->last_reply_header);report(o,"AX211_LAST_REPLY_BYTES",b->last_reply_bytes);
    for(unsigned n=0;n<8;n++)report(o,"AX211_LAST_REPLY_WORD",b->last_reply_words[n]);
}
static void quiesce(WifiBoot *b,const WifiOps *o) {
    interrupt_off(o);
    bits(o,LINK_PWR,0x80000000,0); bits(o,HW_IF,0x18000000,0);
    o->delay(o->opaque,1000); bits(o,LINK_PWR,0,0x80000000); o->delay(o->opaque,5000);
    bits(o,RESET,0x200,0);
    int stopped=poll(o,RESET,0x100,0x100,5000);
    /* Even if STOP_MASTER times out, remove permission before any return. */
    int disabled=o->master(o->opaque,0);
    bits(o,GP,0,12); reset(o); interrupt_off(o);
    b->quiesced=stopped && disabled;
    report(o,b->quiesced?"AX211_QUIESCED":"AX211_STOP_FAILED_DMA_RETAINED",b->used);
}
void wifi_firmware_stop(WifiBoot *b,const WifiOps *o) {
    b->live.active=b->live.info.link=0;
    if(!b->quiesced)quiesce(b,o);
    if(!b->quiesced)return; /* Retain every DMA buffer until stop is confirmed. */
    wifi_wipe(b->live.received,sizeof(b->live.received));
    wifi_wipe(b->live.tx_owned,sizeof(b->live.tx_owned));
    b->live.count=b->live.octets=b->live.tx_count=b->live.tx_octets=0;
    if(b->wpa.enabled) {
        wifi_wipe(b->pool,b->used);
        wifi_wipe(b->wpa.pmk,sizeof(b->wpa.pmk));wifi_wipe(b->wpa.snonce,sizeof(b->wpa.snonce));
        wifi_wipe(b->wpa.anonce,sizeof(b->wpa.anonce));wifi_wipe(b->wpa.ptk,sizeof(b->wpa.ptk));
        wifi_wipe(b->wpa.gtk,sizeof(b->wpa.gtk));wifi_wipe(b->wpa.pending_data,sizeof(b->wpa.pending_data));
        wifi_wipe(&b->rekey,sizeof(b->rekey));
    }
}
const char *wifi_firmware_start(WifiBoot *b,const WifiImage *fw,const WifiOps *o,uint16_t rev) {
    const char *error=wifi_layout(b,fw,rev);
    if(error)return error;
    if(fw->pnvm.data && fw->pnvm_notification!=1)return "unsupported PNVM notification version";
    if(fw->nvm_probe && (!fw->pnvm.data || (fw->nvm_notification!=3 && fw->nvm_notification!=4)))
        return "unsupported/missing NVM probe ABI";
    error=wifi_scan_validate(fw);if(error)return error;
    error=wifi_prepare_validate(fw);if(error)return error;
    error=wifi_auth_validate(fw);if(error)return error;
    error=wifi_assoc_validate(fw);if(error)return error;
    error=wifi_wpa_validate(fw);if(error)return error;
    if(fw->live && !fw->wpa_probe)return "live NIC requires WPA2 profile";
    if(fw->live && fw->beacon_notification!=4)return "live missed-beacon ABI unsupported";
    if(fw->data_probe && !fw->live && (!fw->wpa_probe || !fw->data_target[0] || fw->data_target[0]>=224 || fw->data_target[0]==127))
        return "data probe requires WPA2 and a unicast IPv4 target";
    b->assoc_enabled=fw->assoc_probe;
    if(fw->wpa_probe)wifi_wpa_init(&b->wpa,fw->pmk,fw->snonce);
    /* A live link transfers Ethernet to the IP stack after key installation.
     * The historical zero-SPA ARP diagnostic is only a standalone checkpoint,
     * never a prerequisite for association, live traffic or recovery. */
    if(fw->data_probe && !fw->live) {
        b->data.enabled=1;for(unsigned n=0;n<4;n++)b->data.target[n]=fw->data_target[n];
    }
    unsigned pnvm_locked=0,pnvm_at=0,nvm_at=0;
    b->attempted=1;
    if(!o->master(o->opaque,0))return "PCI bus master disable";
    interrupt_off(o);
    report(o,"AX211_PREPARE",0);
    if(!prepare(o)){error="NIC ready timeout";goto done;}
    reset(o);
    if(!apm(o)){error="MAC clock timeout";goto done;}
    if(!lock(o)){error="interrupt mode NIC lock timeout";goto done;}
    umac(o,0xa05c00,1U<<24); unlock(o); /* legacy CSR causes, PCI IRQs masked */
    if(!(read_reg(o,GP)&0x08000000)){error="hardware radio kill switch";goto done;}
    interrupt_off(o);
    write_reg(o,0x5c,2); write_reg(o,0x5c,4); write_reg(o,INT,UINT32_MAX);
    if(!apm(o)){error="firmware MAC clock timeout";goto done;}
    o->write8(o->opaque,4,0x40); bits(o,0xa8,0x800fffff,0);
    barrier(); /* Publish all DMA contents before addresses or bus mastering. */
    address_reg(o,0x118,b->context); address_reg(o,0x120,b->iml);
    write_reg(o,0x128,fw->iml.size);
    bits(o,HW_IF,2,0);
    write_reg(o,INT_MASK,0x80000001);
    if(!o->master(o->opaque,1)){error="PCI bus master enable";goto done;}
    if(!lock(o)){error="ROM start NIC lock timeout";goto done;}
    umac(o,0xa05c44,1); unlock(o);
    report(o,"AX211_ROM_STARTED",b->used);
    for(unsigned elapsed=0;elapsed<(fw->nvm_probe?15000000U:5000000U);elapsed+=100) {
        uint32_t causes=read_reg(o,INT);
        if(fw->scan_probe && !(read_reg(o,GP)&0x08000000)){error="hardware radio kill during scan";goto diagnosed;}
        if(causes==UINT32_MAX){error="device disappeared";goto diagnosed;}
        if(causes&((1U<<29)|(1U<<25))){error="firmware hardware/software error";goto diagnosed;}
        if(causes)write_reg(o,INT,causes);
        if(causes&1) {
            if(b->csr_alive){error="duplicate ROM ALIVE interrupt";goto diagnosed;}
            b->csr_alive=1;wifi_rx_descriptors(b);barrier();write_reg(o,0x1c80,8);
            report(o,"AX211_ROM_ALIVE",elapsed);
        }
        unsigned producer=dma16(b->status)&0xfff;
        if(producer>=WIFI_RX_COUNT){error="RX producer bounds";goto diagnosed;}
        /* Follow upstream AX210 fixed-order RX buffers. Completion ring is
         * supplied to ROM but not used as a trusted host pointer/index. */
        unsigned before=b->consumed,progress=b->nvm_progress;
        while((b->consumed&511)!=producer) {
            if(!b->csr_alive || b->consumed>=WIFI_RX_COUNT-8){error="startup RX ownership/count";goto diagnosed;}
            unsigned index=b->consumed&511;
            uint8_t *packet=b->rx+index*4096;
            if(b->used_ring[index*32+6]&1){error="fragmented startup RX";goto diagnosed;}
            if(!fw->scan_probe || packet[4]!=0xc1)report(o,"AX211_NOTIFICATION",packet[4]|(unsigned)packet[5]<<8);
            unsigned was_alive=b->alive;
            error=wifi_alive_packet(b,packet,4096,fw->alive_version);
            if(error)goto diagnosed;
            b->last_reply_header=packet[4]|(uint32_t)packet[5]<<8|(uint32_t)packet[6]<<16|(uint32_t)packet[7]<<24;
            b->last_reply_bytes=((packet[0]|(unsigned)packet[1]<<8)&0x3fff)-4;
            for(unsigned n=0;n<8;n++) {
                const uint8_t *v=packet+8+n*4;
                b->last_reply_words[n]=b->last_reply_bytes>=4*n+4?v[0]|(uint32_t)v[1]<<8|(uint32_t)v[2]<<16|(uint32_t)v[3]<<24:0;
            }
            if(was_alive && packet[4]==1 && packet[5]==0){error="duplicate firmware ALIVE";goto diagnosed;}
            error=wifi_pnvm_packet(b,packet,4096);
            if(error)goto diagnosed;
            unsigned had_nvm=b->nvm_complete,reg_before=b->reg_notifications,applied_before=b->reg_applied_generation;
            error=wifi_nvm_packet(b,fw,packet,4096);
            if(error)goto diagnosed;
            if(reg_before!=b->reg_notifications) {
                report(o,"AX211_REG_COUNTRY_DETECTED",b->reg_country);report(o,"AX211_REG_COUNTRY_SOURCE",b->reg_source);
            }
            if(applied_before!=b->reg_applied_generation) {
                report(o,"AX211_REG_PROFILE_REFRESHED",b->reg_refreshes);report(o,"AX211_REG_FILTERED_APS",b->reg_filtered);
            }
            ++b->consumed;
            if(packet[4]==0xfe && packet[5]==12 && b->pnvm_complete) {
                report(o,"AX211_PNVM_STATUS",b->pnvm_status);
                report(o,"AX211_PNVM_COMPLETE",b->pnvm_status);
                if(!fw->nvm_probe)goto done;
                nvm_at=elapsed;
            }
            if(b->nvm_complete && !had_nvm) {
                report(o,"AX211_INIT_COMPLETE",b->init_complete);
                report(o,"AX211_NVM_VERSION",b->nvm_version);
                report(o,"AX211_NVM_CHANNELS",b->nvm_channels);
                report(o,"AX211_NVM_VALID_CHANNELS",b->nvm_valid_channels);
                report(o,"AX211_NVM_TX_CHAINS",b->nvm_tx_chains);
                report(o,"AX211_NVM_RX_CHAINS",b->nvm_rx_chains);
                report(o,"AX211_NVM_MAC_FLAGS",b->nvm_mac_flags);
                report(o,"AX211_NVM_GENERAL_FLAGS",b->nvm_general_flags);
                report(o,"AX211_NVM_LAR",b->nvm_lar);
                if(!fw->scan_probe)goto done;
            }
            if(b->alive && !was_alive) {
                report(o,"AX211_FW_ALIVE",fw->alive_version);
                report(o,"AX211_LMAC_MAJOR",b->lmac_major); report(o,"AX211_LMAC_MINOR",b->lmac_minor);
                report(o,"AX211_UMAC_MAJOR",b->umac_major); report(o,"AX211_UMAC_MINOR",b->umac_minor);
                for(unsigned n=0;n<3;n++)report(o,"AX211_SKU_WORD",b->sku[n]);
                if(!fw->pnvm.data)goto done;
                error=wifi_pnvm_layout(b,fw);
                if(error)goto diagnosed;
                report(o,"AX211_PNVM_VERSION",b->pnvm_version);
                report(o,"AX211_PNVM_BYTES",b->pnvm_size);
                report(o,"AX211_PNVM_FRAGMENTS",b->pnvm_count);
                if(!lock(o)){error="PNVM doorbell NIC lock timeout";goto diagnosed;}
                pnvm_locked=1;b->pnvm_started=1;pnvm_at=elapsed;
                barrier();umac(o,0xa05c04,1U<<20);
                report(o,"AX211_PNVM_SUBMITTED",fw->fragmented_pnvm);

            }
            /* Clear before returning this buffer to the fixed-order ring:
             * live reuse must not walk stale trailing notifications. */
            wifi_wipe(packet,4096);packet[2]=0x55;packet[3]=0x55;
        }
        if(b->consumed!=before) {
            barrier();write_reg(o,0x1c80,((producer?producer:512)-1)&~7U);
        }
        if(causes&0x90000008) {
            write_reg(o,FH_INT,(1U<<17)|(1U<<16)|(1U<<30));
            o->write8(o->opaque,5,0);
        }
        error=wifi_nvm_advance(b,fw,o);
        if(error)goto diagnosed;
        error=wifi_scan_advance(b,fw,o);
        if(error)goto diagnosed;
        error=wifi_prepare_advance(b,fw,o);
        if(error)goto diagnosed;
        error=wifi_auth_advance(b,fw,o);
        if(error)goto diagnosed;
        error=wifi_assoc_advance(b,fw,o);
        if(error)goto diagnosed;
        error=wifi_wpa_advance(b,fw,o);if(error)goto diagnosed;
        if(!fw->live){error=wifi_data_advance(b,fw,o);if(error)goto diagnosed;}
        if(b->scan_complete && b->scan_acked && !b->command_pending && b->reg_generation==b->reg_applied_generation &&
           (!fw->prepare_ssid_size || b->prepare_complete) &&
           (!fw->auth_probe || (b->auth_tx_done && b->auth_received)) &&
           (!fw->assoc_probe || b->assoc_complete) && (!fw->wpa_probe || b->wpa.complete) &&
           (!fw->data_probe || fw->live || b->data.complete)) {
            report(o,"AX211_SCAN_AP_COUNT",b->scan_count);
            if(b->prepare_complete)report(o,"AX211_STATION_SETUP_COMPLETE",1);
            if(fw->live) {
                error=wifi_live_start(b);if(error)goto diagnosed;
                if(pnvm_locked)unlock(o);
                return 0; /* Explicit live ownership transfers to nic_stop. */
            }
            goto done;
        }
        if(b->nvm_progress!=progress)nvm_at=elapsed;
        if(fw->nvm_probe && b->pnvm_complete && elapsed-nvm_at>=2000000) {
            error=b->command_pending?"firmware command reply timeout":b->data.sent?"protected ARP reply timeout":b->wpa.enabled && b->assoc_complete?"WPA2 handshake/key timeout":b->assoc_sent?"association exchange/update timeout":b->auth_enabled?"authentication exchange timeout":b->scan_started?"passive scan completion timeout":"firmware init notification timeout";goto diagnosed;
        }
        if(b->pnvm_started && !b->pnvm_complete && elapsed-pnvm_at>=2000000){error="PNVM completion timeout";goto diagnosed;}
        if(elapsed && elapsed%1000000==0)diagnostics(b,o);
        o->delay(o->opaque,100);
    }
    error=b->alive?"firmware startup total timeout":"firmware ALIVE timeout";
diagnosed:
    diagnostics(b,o);
done:
    if(pnvm_locked)unlock(o);
    wifi_firmware_stop(b,o);
    if(!b->quiesced)return "device stop unconfirmed (DMA memory retained)";
    return error;
}
