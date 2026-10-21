/* Included after WifiAp by wifi.h. One bounded WPA2-PSK/CCMP handshake. */
#ifndef RIPLIX_WIFI_WPA_H
#define RIPLIX_WIFI_WPA_H
#include "wifi-crypto.h"
typedef struct {
    unsigned enabled,m1_seen,m3_seen,m2_count,m4_count,pending,pending_bytes;
    unsigned tx_next,tx_wait,tx_kind,tx_status,m4_done,key_step,key_installed,complete;
    unsigned gtk_id,gtk_bytes,last_info,last_bytes,duplicates;
    uint64_t m1_replay,m3_replay,gtk_rsc;
    uint8_t pmk[32],snonce[32],anonce[32],ptk[48],gtk[32],pending_data[128];
} WifiWpa;
void wifi_wpa_init(WifiWpa *,const uint8_t[32],const uint8_t[32]);
const char *wifi_wpa_input(WifiWpa *,const uint8_t[6],const WifiAp *,const uint8_t *,unsigned);
#endif
