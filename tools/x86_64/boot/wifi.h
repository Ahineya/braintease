/* AX211 startup-only transport. Wire ABI and sequence derived from OpenBSD
 * if_iwx.c v1.230 / if_iwxreg.h v1.75; see wifi-LICENSE.txt for notices.
 * Native octets/addresses throughout; no C16 memory is exposed to DMA. */
#ifndef RIPLIX_WIFI_H
#define RIPLIX_WIFI_H
#include <stdint.h>
#include <stddef.h>
#include "net.h"
#include "wifi-live-diag.h"
#define WIFI_DMA_BYTES (6U * 1024 * 1024)
#define WIFI_RX_COUNT 512U
#define WIFI_CONTEXT_BYTES 104U
#define WIFI_SCRATCH_BYTES 1660U
/* One initial passive scan plus at most two recovery/profile rescans. */
#define WIFI_SCAN_ATTEMPTS_MAX 3U
typedef struct { const uint8_t *data; unsigned size; } WifiSection;
typedef struct {
    WifiSection section[3][64], iml, pnvm; /* LMAC, UMAC, paging */
    unsigned count[3], alive_version, fragmented_pnvm, pnvm_notification, nvm_probe, nvm_notification;
    uint32_t version[3], capabilities[8], api[8];
    unsigned scan_probe, scan_cfg_version, scan_request_version, rx_notification, mcc_notification;
    unsigned phy_version, rlc_version, mac_version, link_version, sta_version, queue_version;
    uint8_t prepare_ssid[32];
    unsigned prepare_ssid_size;
    unsigned assoc_probe, wpa_probe, key_version, data_probe, live, beacon_notification;
    uint8_t data_target[4]; /* temporary ARP probe target, no claimed IPv4 */
    uint8_t pmk[32], snonce[32];
    unsigned auth_probe, tx_version, tx_notification, session_version, session_notification;
} WifiImage;
typedef struct {
    WifiSection section[64];
    unsigned count, size;
    uint32_t version;
} WifiPnvm;
typedef struct {
    void *opaque;
    uint32_t (*read)(void *, unsigned);
    void (*write)(void *, unsigned, uint32_t);
    void (*write8)(void *, unsigned, uint8_t);
    void (*delay)(void *, unsigned); /* microseconds, calibrated by platform */
    int (*master)(void *, int); /* set PCI bus mastering, verify; IRQs stay masked */
    void (*log)(void *, const char *, uint64_t);
} WifiOps;
typedef struct {
    uint8_t bssid[6], ssid[32];
    unsigned ssid_size, channel, energy, privacy, rsn;
    unsigned capabilities, beacon_interval, dtim_period;
    unsigned rates, basic_rates, unknown_basic, rsn_caps, group_cipher, pair_ciphers, akms;
    unsigned details_valid;
    uint8_t rsn_ie[257];unsigned rsn_size;
    uint32_t group_suite; /* raw OUI/type, LE octets, for unknown-suite diagnosis */
} WifiAp;
#include "wifi-wpa.h"
#define WIFI_ETH_FRAME 1514U
typedef struct {
    unsigned enabled,sent,tx_wait,tx_status,reply,complete,rx_packets,drops,replays,group_mic_bad;
    uint32_t last_status;
    uint64_t rx_pn[16],group_pn[16],last_pn;
    uint8_t target[4],peer[6];
} WifiData;
typedef struct {
    unsigned active,head,count,octets,tx_head,tx_tail,tx_count,tx_octets,sequence;
    unsigned last_ssn; /* tx_head/tail are 16-bit hardware counters; DMA index is &255. */
    uint32_t ignored_ssn;
    uint8_t *tx_pages;
    unsigned tx_size[256];uint8_t tx_owned[256];uint64_t tx_when[256];
    uint8_t received[NIC_FRAMES][NIC_FRAME];unsigned lengths[NIC_FRAMES];
    NicInfo info;
    WifiLiveDiag diag;
} WifiLive;
typedef struct {
    unsigned valid,bytes;
    uint8_t key[32];
    uint64_t pn[16];
} WifiGroupKey;
typedef struct {
    WifiGroupKey keys[4];
    unsigned stage,kind,key_id,key_bytes,changed,command_index,tx_wait,retries;
    uint32_t rotations,duplicates,rejected;
    uint64_t replay,deadline,rsc;
    uint8_t key[32],digest[20],response[99];
    uint8_t *command_page;
} WifiRekey;
typedef struct {
    uint8_t *pool;
    unsigned used, capacity;
    uint8_t *context, *scratch, *info, *status, *free_ring, *used_ring, *rx, *tx, *iml;
    uint32_t sku[3], lmac_major, lmac_minor, umac_major, umac_minor;
    unsigned consumed, csr_alive, alive, quiesced;
    unsigned pnvm_started, pnvm_complete, pnvm_count, pnvm_size;
    uint32_t pnvm_version, pnvm_status;
    uint8_t *command_data;
    unsigned command_count, command_pending, command_code, command_group, nvm_progress;
    unsigned init_complete, nvm_complete, nvm_version, nvm_channels, nvm_valid_channels;
    unsigned nvm_tx_chains, nvm_rx_chains, nvm_mac_flags, nvm_addresses;
    uint32_t nvm_general_flags, nvm_lar; /* informational/raw NVM fields */
    uint32_t nvm_channel_flags[18];
    uint32_t mcc_channel_flags[18];
    uint8_t mac[6];
    unsigned mac_valid, radio_step, regulatory_done, scan_started, scan_acked, scan_complete;
    unsigned scan_channels, scan_channels_5, scan_count, scan_dropped, scan_frames, scan_bad_frames;
    unsigned scan_attempts, scan_uid, scan_rescan_pending;
    unsigned scan_requested_channels, scan_requested_channels_5;
    uint8_t scan_target_ssid[32];unsigned scan_target_size;
    unsigned reg_country, reg_source, reg_notifications, reg_generation;
    unsigned reg_applied_generation, reg_query_generation, reg_refreshes, reg_filtered;
    unsigned reg_applied_country, reg_applied_source, reg_profile_status;
    unsigned reg_profile_aux; /* opaque reply bytes13..15, never permissions */
    WifiAp aps[32];
    WifiAp target; /* frozen selection; later beacons cannot retarget setup */
    unsigned prepare_started, prepare_step, prepare_complete, management_qid;
    unsigned management_aux; /* opaque queue response padding */
    unsigned management_flags; /* retained raw; not a TX success indication */
    uint8_t *management_tx, *management_bc;
    unsigned auth_enabled, auth_phase, auth_session, auth_sent, auth_tx_done, auth_received;
    unsigned auth_tx_status, auth_status;
    unsigned assoc_enabled, assoc_sent, assoc_tx_done, assoc_received, assoc_step, assoc_complete;
    unsigned assoc_tx_status, assoc_status, assoc_aid, assoc_caps, assoc_rates, assoc_basic;
    WifiWpa wpa;
    WifiData data;
    WifiLive live;
    uint32_t last_reply_header, last_reply_bytes, last_reply_words[8];
    int attempted; /* one shot: never reuse memory if stop failed */
    WifiRekey rekey;
} WifiBoot;
const char *wifi_image_parse(const uint8_t *, size_t, WifiImage *);
const char *wifi_layout(WifiBoot *, const WifiImage *, uint16_t);
void wifi_rx_descriptors(WifiBoot *);
void *wifi_dma_reserve(WifiBoot *, unsigned);
const char *wifi_pnvm_select(const uint8_t *, size_t, const uint32_t[3], unsigned, unsigned, WifiPnvm *);
const char *wifi_pnvm_layout(WifiBoot *, const WifiImage *);
const char *wifi_pnvm_packet(WifiBoot *, const uint8_t *, size_t);
const char *wifi_nvm_parse(WifiBoot *, const uint8_t *, unsigned, unsigned);
const char *wifi_nvm_packet(WifiBoot *, const WifiImage *, const uint8_t *, unsigned);
const char *wifi_nvm_advance(WifiBoot *, const WifiImage *, const WifiOps *);
/* Validates a single native RX buffer; unknown valid notifications return 0. */
const char *wifi_alive_packet(WifiBoot *, const uint8_t *, size_t, unsigned);
/* Bounded profiles stop on success/failure. Explicit live success transfers
 * ownership to wifi_firmware_stop. Caller keeps the pool reserved forever. */
const char *wifi_firmware_start(WifiBoot *, const WifiImage *, const WifiOps *, uint16_t);
const char *wifi_scan_validate(const WifiImage *);
const char *wifi_scan_advance(WifiBoot *, const WifiImage *, const WifiOps *);
const char *wifi_scan_reply(WifiBoot *, const uint8_t *, unsigned);
const char *wifi_scan_notification(WifiBoot *, const uint8_t *, unsigned);
const char *wifi_scan_mpdu(WifiBoot *, const uint8_t *, unsigned);
int wifi_mac_valid(const uint8_t *);
const char *wifi_command(WifiBoot *, const WifiOps *, unsigned, unsigned, unsigned, const uint8_t *, unsigned);
const char *wifi_ap_details(WifiAp *, const uint8_t *, unsigned);
unsigned wifi_rsn_request(const WifiAp *,uint8_t *);
const char *wifi_prepare_validate(const WifiImage *);
const char *wifi_prepare_advance(WifiBoot *, const WifiImage *, const WifiOps *);
const char *wifi_prepare_reply(WifiBoot *, const uint8_t *, unsigned);
const char *wifi_target_select(WifiBoot *, const WifiImage *);
const char *wifi_auth_validate(const WifiImage *);
const char *wifi_auth_advance(WifiBoot *, const WifiImage *, const WifiOps *);
const char *wifi_auth_reply(WifiBoot *, const uint8_t *, unsigned);
const char *wifi_auth_tx_reply(WifiBoot *, const uint8_t *, unsigned);
const char *wifi_auth_notification(WifiBoot *, const uint8_t *, unsigned);
const char *wifi_auth_mpdu(WifiBoot *, const uint8_t *, unsigned);
const char *wifi_assoc_validate(const WifiImage *);
const char *wifi_assoc_advance(WifiBoot *, const WifiImage *, const WifiOps *);
const char *wifi_assoc_reply(WifiBoot *, const uint8_t *, unsigned);
const char *wifi_assoc_tx_reply(WifiBoot *, const uint8_t *, unsigned);
const char *wifi_assoc_mpdu(WifiBoot *, const uint8_t *, unsigned);
const char *wifi_wpa_validate(const WifiImage *);
const char *wifi_wpa_advance(WifiBoot *,const WifiImage *,const WifiOps *);
const char *wifi_wpa_reply(WifiBoot *,const uint8_t *,unsigned);
const char *wifi_wpa_tx_reply(WifiBoot *,const uint8_t *,unsigned);
const char *wifi_wpa_mpdu(WifiBoot *,const uint8_t *,unsigned);
/* Bounded encrypted unicast checkpoint, no persistent NIC/IP interface. */
const char *wifi_data_advance(WifiBoot *,const WifiImage *,const WifiOps *);
const char *wifi_data_tx_reply(WifiBoot *,const uint8_t *,unsigned);
const char *wifi_data_mpdu(WifiBoot *,const uint8_t *,unsigned);
unsigned wifi_data_decode(WifiBoot *,const uint8_t *,unsigned,uint8_t[WIFI_ETH_FRAME],unsigned *);
unsigned wifi_data_decode_flags(WifiBoot *,const uint8_t *,unsigned,uint8_t[WIFI_ETH_FRAME],unsigned *,uint32_t);
unsigned wifi_data_encode(const WifiBoot *,const uint8_t *,unsigned,uint8_t *,unsigned);
void wifi_michael(const uint8_t[8],const uint8_t[6],const uint8_t[6],unsigned,const uint8_t *,unsigned,uint8_t[8]);
const char *wifi_live_start(WifiBoot *);
const char *wifi_live_poll(WifiBoot *,const WifiOps *,uint64_t);
unsigned wifi_live_tx(WifiBoot *,const WifiOps *,const uint8_t *,unsigned,uint64_t);
unsigned wifi_live_rx(WifiBoot *,uint8_t *,unsigned,unsigned *);
void wifi_live_info(WifiBoot *,NicInfo *);
const char *wifi_rekey_start(WifiBoot *);
const char *wifi_rekey_input(WifiBoot *,const uint8_t *,unsigned,uint64_t);
const char *wifi_rekey_poll(WifiBoot *,const WifiOps *,uint64_t);
const char *wifi_rekey_reply(WifiBoot *,const uint8_t *,unsigned);
const char *wifi_rekey_tx_done(WifiBoot *,const WifiOps *,unsigned,unsigned);
unsigned wifi_live_control_tx(WifiBoot *,const WifiOps *,const uint8_t *,unsigned,uint64_t);
void wifi_firmware_stop(WifiBoot *,const WifiOps *);
void wifi_bringup(void);
#endif
