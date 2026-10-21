/* Public independent hashlib/OpenSSL answers, including authenticated bad key
 * data. No production credentials. Native production supplicant under UBSan. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../boot/wifi.h"
#include "fixtures/wifi-wpa-vectors.h"
static WifiAp fixture(unsigned group) {
 WifiAp ap={.channel=48,.group_cipher=group==2?4:16};uint8_t ies[32]={1,1,0x8c};
 const uint8_t *rsn=group==2?vector_rsn_2:vector_rsn_4;
 memcpy(ies+3,rsn,22);assert(!wifi_ap_details(&ap,ies,25));memcpy(ap.bssid,vector_ap,6);return ap;
}
static WifiWpa start(const WifiAp *ap) {
 WifiWpa w;wifi_wpa_init(&w,vector_pmk,vector_snonce);
 assert(!wifi_wpa_input(&w,vector_own,ap,vector_m1_2,sizeof(vector_m1_2)));
 assert(w.m1_seen && !w.m3_seen && w.pending==2 && w.pending_bytes==121);
 const uint8_t *m2=ap->group_cipher==4?vector_m2_2:vector_m2_4;
 assert(!memcmp(w.ptk,vector_ptk,48) && !memcmp(w.pending_data,m2,121));w.m2_count=1;w.pending=0;return w;
}
int main(void) {
 unsigned cases=0;
 for(unsigned group=2;group<=4;group+=2) {
  WifiAp ap=fixture(group);WifiWpa initial=start(&ap);
  const uint8_t *m3=group==2?vector_m3_2:vector_m3_4;
  unsigned bytes=group==2?sizeof(vector_m3_2):sizeof(vector_m3_4),gtk_n=group==2?32:16;
  WifiWpa good=initial;assert(!wifi_wpa_input(&good,vector_own,&ap,m3,bytes));
  assert(good.m3_seen && good.pending==4 && good.gtk_bytes==gtk_n && good.gtk_id==1 && good.m3_replay==8);
  assert(!memcmp(good.pending_data,vector_m4_2,sizeof(vector_m4_2)));
  assert(!memcmp(good.gtk,group==2?vector_gtk_2:vector_gtk_4,gtk_n));++cases;
  good.key_step=good.key_installed=2;good.gtk_rsc=123;good.m4_count=1;
  assert(!wifi_wpa_input(&good,vector_own,&ap,m3,bytes));
  assert(good.key_step==2 && good.key_installed==2 && good.gtk_rsc==123 && good.pending==4 && good.duplicates==1);++cases;
  const uint8_t *bad_rsn=group==2?vector_bad_rsn_2:vector_bad_rsn_4;
  const uint8_t *bad_wrap=group==2?vector_bad_wrap_2:vector_bad_wrap_4;
  for(unsigned n=0;n<bytes;n++) {
   WifiWpa cut=initial;assert(wifi_wpa_input(&cut,vector_own,&ap,m3,n));assert(!cut.m3_seen && !cut.key_installed);++cases;
  }
  uint8_t p[611];
  for(unsigned n=0;n<8;n++) {
   WifiWpa bad=initial;memcpy(p,m3,bytes);
   switch(n) {
    case 0:p[81]^=1;break;case 1:p[17]^=1;break;case 2:p[5]&=~0x10;break;
    case 3:p[8]=32;break;case 4:p[4]=254;break;case 5:p[6]=0xc9;break;
    case 6:memcpy(p,bad_rsn,bytes);break;case 7:memcpy(p,bad_wrap,bytes);break;
   }
   const char *why=wifi_wpa_input(&bad,vector_own,&ap,p,bytes);
   assert(why && !bad.m3_seen && !bad.key_installed);++cases;
  }
  /* Discard old counters without resetting or installing keys. */
  WifiWpa stale=initial;memcpy(p,m3,bytes);memset(p+9,0,8);
  assert(!wifi_wpa_input(&stale,vector_own,&ap,p,bytes) && !stale.m3_seen);++cases;
  WifiWpa retry=initial;assert(!wifi_wpa_input(&retry,vector_own,&ap,vector_m1_2,sizeof(vector_m1_2)));
  assert(retry.duplicates==1 && retry.pending==2 && !memcmp(retry.ptk,vector_ptk,48));++cases;
 }
 printf("AX211_WPA_PROTOCOL_HOST_OK %u public oracle/replay/RSN/MIC/wrap/truncation cases\n",cases);
}
