/* Same full production startup/scan/auth/assoc/WPA/key state machine. Public
 * independent EAPOL vectors and exact upstream key ABI, fake native MMIO. */
#define WIFI_ASSOC_MAIN wifi_assoc_tests
#include "wifi-assoc.c"
#include "fixtures/wifi-wpa-vectors.h"
enum {WPA_OK,WPA_EARLY_M1,WPA_M3_FIRST,WPA_M1_RETRY,WPA_M3_RETRY,WPA_M3_AFTER_PTK,
 WPA_QOS_PAD,WPA_OTHER_ETH_SOURCE,WPA_OPAQUE_ACK,WPA_SESSION_END,WPA_NO_M1,WPA_NO_M3,WPA_M2_FAIL,
 WPA_M4_FAIL,WPA_BAD_MIC,WPA_BAD_RSN,WPA_BAD_WRAP,WPA_M3_OLD,WPA_BAD_QID,WPA_LAST};
static unsigned wpa_fault,key_fault,key_index,key_calls[2];
static uint32_t wpa_read(void *opaque,unsigned reg) {
 if(reg==0x388)return 0x02030405;if(reg==0x38c)return 0x0607;
 return scan_read(opaque,reg);
}
static void ap_eapol(uint8_t *p,const WifiBoot *b,unsigned kind) {
 memset(p,0,4096);unsigned pad=wpa_fault==WPA_QOS_PAD?2:0,header=wpa_fault==WPA_QOS_PAD?26:24;
 const uint8_t *e=kind==1?vector_m1_2:beacon_group==2?vector_m3_2:vector_m3_4;
 unsigned size=kind==1?sizeof(vector_m1_2):beacon_group==2?sizeof(vector_m3_2):sizeof(vector_m3_4);
 if(kind==3 && wpa_fault==WPA_BAD_RSN)e=beacon_group==2?vector_bad_rsn_2:vector_bad_rsn_4;
 if(kind==3 && wpa_fault==WPA_BAD_WRAP)e=beacon_group==2?vector_bad_wrap_2:vector_bad_wrap_4;
 unsigned len=header+pad+8+size;p32(p,4+64+len);p[4]=0xc1;p[7]=0x80;
 uint8_t *d=p+8,*f=d+64;d[0]=len;d[1]=len>>8;d[3]=pad?0x20:0;p32(d+12,3);d[42]=48;
 f[0]=header==26?0x88:8;f[1]=2;
 memcpy(f+4,b->mac,6);memcpy(f+10,b->target.bssid,6);memcpy(f+16,b->target.bssid,6);
 if(wpa_fault==WPA_OTHER_ETH_SOURCE)f[16]^=2;
 const uint8_t llc[]={0xaa,0xaa,3,0,0,0,0x88,0x8e};memcpy(f+header+pad,llc,8);
 memcpy(f+header+pad+8,e,size);
 if(kind==3 && wpa_fault==WPA_BAD_MIC)f[header+pad+8+81]^=1;
 if(kind==3 && wpa_fault==WPA_M3_OLD)memset(f+header+pad+8+9,0,8);
}
static void wpa_completion(uint8_t *p,unsigned ssn,unsigned kind) {
 tx_completion(p);p32(p+8+44,ssn);
 if((wpa_fault==WPA_M2_FAIL && kind==2) || (wpa_fault==WPA_M4_FAIL && kind==4))p[8+40]=0x82;
 if(wpa_fault==WPA_BAD_QID)p[8+36]=2;
}
static void wpa_write(void *opaque,unsigned reg,uint32_t v) {
 Device *d=opaque;WifiBoot *b=d->b;WifiWpa *w=&b->wpa;
 if(reg==0x460 && v>=0x10003) {
  unsigned ssn=v&65535,index=ssn-1;assert(ssn==w->tx_wait && d->master && b->assoc_complete);
  uint8_t *desc=b->management_tx+index*256,*cmd=(void *)(uintptr_t)u64(desc+4),*f=cmd+32;
  assert(cmd[0]==0x1c && cmd[2]==index && cmd[3]==1 && (u32(cmd+4)>>16)==7);
  assert(f[0]==8 && f[1]==1 && (f[22]|(unsigned)f[23]<<8)==index*16);
  assert(!memcmp(f+4,vector_ap,6) && !memcmp(f+10,vector_own,6) && !memcmp(f+16,vector_ap,6));
  const uint8_t *expected=w->tx_kind==2?(beacon_group==2?vector_m2_2:vector_m2_4):vector_m4_2;
  unsigned size=w->tx_kind==2?121:99;
  assert(!memcmp(cmd+72,expected,size) && (u32(cmd+4)&65535)==32+size);
  assert((u32(desc)&65535)==3 && u32(desc)>>16==20 && (u32(desc+12)&65535)==36);
  assert(u64(desc+14)==(uintptr_t)cmd+20 && u64(desc+24)==(uintptr_t)cmd+64 && (u32(desc+22)&65535)==size+8);
  assert((b->management_bc[index*2]|(unsigned)b->management_bc[index*2+1]<<8)==32+size);
  unsigned n=b->consumed;
  if(wpa_fault==WPA_SESSION_END && w->tx_kind==2) {
   uint8_t *notice=b->rx+n++*4096;memset(notice,0,4096);p32(notice,20);
   notice[4]=0xfb;notice[5]=3;notice[7]=0x80;p32(notice+12,1);
  }
  if(w->tx_kind==2 && wpa_fault==WPA_M3_FIRST)ap_eapol(b->rx+n++*4096,b,3);
  wpa_completion(b->rx+n++*4096,ssn,w->tx_kind);
  if(w->tx_kind==2 && wpa_fault!=WPA_NO_M3 && wpa_fault!=WPA_M3_FIRST) {
   unsigned kind=wpa_fault==WPA_M1_RETRY && w->m2_count==1?1:3;
   ap_eapol(b->rx+n++*4096,b,kind);
  }
  if(w->tx_kind==4 && wpa_fault==WPA_M3_RETRY && w->m4_count==1)ap_eapol(b->rx+n++*4096,b,3);
  *(uint16_t *)(void *)b->status=n;return;
 }
 if(reg==0x460 && b->command_group==5 && b->command_code==24) {
  unsigned step=w->key_step;assert(step<2 && w->m4_done && w->m3_seen && d->master);++key_calls[step];
  uint8_t expected[80]={0},*cmd=b->command_data;assert(cmd[0]==24 && cmd[1]==5 && u32(cmd+4)==80);
  p32(expected,1);p32(expected+4,1);p32(expected+8,step?1:0);p32(expected+12,step?(beacon_group==2?0x43:0x42):2);
  if(!step)memcpy(expected+16,vector_ptk+32,16);
  else if(beacon_group==4)memcpy(expected+16,vector_gtk_4,16);
  else {
   memcpy(expected+16,vector_gtk_2,16);memcpy(expected+32,vector_gtk_2+24,8);memcpy(expected+40,vector_gtk_2+16,8);
   memcpy(expected+48,vector_gtk_2+16,8);memcpy(expected+56,vector_gtk_2+24,8);
  }
  assert(!memcmp(cmd+8,expected,80));
  if(step==key_index && key_fault==1)return;
  unsigned n=b->consumed;uint8_t *ack=b->rx+n++*4096;memset(ack,0,4096);p32(ack,4);ack[4]=24;ack[5]=5;ack[6]=v-1;
  if(step==key_index && key_fault==2)ack[5]|=0x40;
  if(step==key_index && key_fault==3)ack[6]=99;
  if(wpa_fault==WPA_OPAQUE_ACK){p32(ack,8);p32(ack+8,0xdeadbeef);}
  if(wpa_fault==WPA_M3_AFTER_PTK && !step)ap_eapol(b->rx+n++*4096,b,3);
  *(uint16_t *)(void *)b->status=n;return;
 }
 assoc_write(opaque,reg,v);
 if(reg==0x460 && v==12) {
  unsigned n=*(uint16_t *)(void *)b->status;
  for(unsigned i=b->consumed;i<n;i++) {
   uint8_t *p=b->rx+i*4096;
   if(p[4]==0xc1 && p[8+42]==48){memcpy(p+8+64+10,vector_ap,6);memcpy(p+8+64+16,vector_ap,6);}
  }
 }
 if(reg==0x460 && b->assoc_sent && b->assoc_step==2 && wpa_fault!=WPA_NO_M1) {
  unsigned n=*(uint16_t *)(void *)b->status;
  if(wpa_fault==WPA_EARLY_M1){memcpy(b->rx+n*4096,b->rx+b->consumed*4096,4096);ap_eapol(b->rx+b->consumed*4096,b,1);n++;}
  else ap_eapol(b->rx+n++*4096,b,1);
  *(uint16_t *)(void *)b->status=n;
 }
}
#ifndef WIFI_WPA_TRANSPORT_MAIN
#define WIFI_WPA_TRANSPORT_MAIN main
#endif
int WIFI_WPA_TRANSPORT_MAIN(int argc,char **argv) {
 assert(!wifi_assoc_tests(argc,argv));
 FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);blob=malloc(size);assert(blob && fread(blob,1,size,f)==(size_t)size);fclose(f);
 assert(!wifi_image_parse(blob,size,&image));f=fopen(argv[2],"rb");assert(f);fseek(f,0,SEEK_END);size=ftell(f);rewind(f);
 uint8_t *pnvm=malloc(size);assert(pnvm && fread(pnvm,1,size,f)==(size_t)size);fclose(f);
 image.pnvm=(WifiSection){pnvm,(unsigned)size};image.nvm_probe=image.scan_probe=image.auth_probe=image.assoc_probe=image.wpa_probe=1;
 image.prepare_ssid_size=3;memcpy(image.prepare_ssid,"abc",3);memcpy(image.pmk,vector_pmk,32);memcpy(image.snonce,vector_snonce,32);
 assert(image.key_version==1);auth_fault=AUTH_OK;assoc_fault=ASSOC_OK;scan_fault=SCAN_OK;fault_kind=update_fault=0;expected_basic_ofdm=0x15;
 unsigned cases=0;
 for(unsigned n=0;n<2;n++) {
  WifiImage bad=image;if(n)bad.assoc_probe=0;else bad.key_version=0;
  WifiBoot b={.pool=pool,.capacity=sizeof(pool)};Device d={.b=&b};
  WifiOps ops={&d,wpa_read,wpa_write,write_byte,delay,mastering,prepare_log};
  assert(wifi_firmware_start(&b,&bad,&ops,0x441) && !d.writes && !b.attempted);++cases;
 }
 for(beacon_group=2;beacon_group<=4;beacon_group+=2)for(wpa_fault=0;wpa_fault<WPA_LAST;wpa_fault++) {
  WifiBoot b={.pool=pool,.capacity=sizeof(pool)};Device d={.b=&b,.fault=NVM_OPAQUE_STATUS};
  WifiOps ops={&d,wpa_read,wpa_write,write_byte,delay,mastering,prepare_log};key_calls[0]=key_calls[1]=0;
  const char *error=wifi_firmware_start(&b,&image,&ops,0x441);unsigned success=wpa_fault<WPA_NO_M1;
  if((error==0)!=success)fprintf(stderr,"wpa %u/%u: %s\n",beacon_group,wpa_fault,error?error:"unexpected success");
  assert((error==0)==success && b.quiesced && !d.master);
  if(success)assert(b.wpa.complete && b.wpa.m4_done && b.wpa.key_installed==2 && key_calls[0]==1 && key_calls[1]==1);
  else assert(!b.wpa.complete);
  for(unsigned n=0;n<b.used;n++)assert(!b.pool[n]);
  for(unsigned n=0;n<48;n++)assert(!b.wpa.ptk[n]);
  ++cases;
 }
 wpa_fault=WPA_OK;beacon_group=2;
 for(key_index=0;key_index<2;key_index++)for(key_fault=1;key_fault<=3;key_fault++) {
  WifiBoot b={.pool=pool,.capacity=sizeof(pool)};Device d={.b=&b,.fault=NVM_OPAQUE_STATUS};WifiOps ops={&d,wpa_read,wpa_write,write_byte,delay,mastering,prepare_log};
  assert(wifi_firmware_start(&b,&image,&ops,0x441) && !b.wpa.complete && b.quiesced && !d.master);++cases;
 }
 free(blob);free(pnvm);printf("AX211_WPA_TRANSPORT_HOST_OK %u startup/EAPOL/key/retry/stop cases; RF untested\n",cases);return 0;
}
