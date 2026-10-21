/* Production live NIC against fake MMIO/DMA, including real startup handoff.
 * Recycles >512 RX buffers and wraps 256 TX slots; does not certify RF. */
#define WIFI_WPA_TRANSPORT_MAIN startup_tests
#include "wifi-wpa-transport.c"
static unsigned hardware_write_pointer;
static void live_write(void *opaque,unsigned reg,uint32_t value) {
 Device *d=opaque;
 if(d->b->live.active && reg==0x460){
  /* Independent device model: AX210 hardware pointer wraps at 65536, NOT
   * at the host DMA ring's 256 entries. Do not derive this from host head. */
  hardware_write_pointer=(hardware_write_pointer+1)&65535;
  if(value!=(0x10000U|hardware_write_pointer))fprintf(stderr,"hardware write-pointer mismatch: got %u expected %u\n",value&65535,hardware_write_pointer);
  assert(value==(0x10000U|hardware_write_pointer));
  d->reg[reg/4]=value;++d->writes;return;
 }
 write_reg(opaque,reg,value);
}
static WifiOps live_ops(Device *d){return (WifiOps){d,wpa_read,live_write,write_byte,delay,mastering,prepare_log};}
static void start_live(WifiBoot *b,Device *d) {
 *b=(WifiBoot){.pool=pool,.capacity=sizeof(pool)};*d=(Device){.b=b,.fault=NVM_OPAQUE_STATUS};
 WifiOps ops={d,wpa_read,wpa_write,write_byte,delay,mastering,prepare_log};
 const char *error=wifi_firmware_start(b,&image,&ops,0x441);
 if(error)fprintf(stderr,"live startup: %s\n",error);
 assert(!error && b->live.active && b->live.info.link && !b->quiesced && d->master);
 assert(b->used<=b->capacity && b->live.tx_head==b->wpa.tx_next);
 hardware_write_pointer=b->wpa.tx_next;
}
static void stop_live(WifiBoot *b,Device *d) {
 WifiOps ops=live_ops(d);wifi_firmware_stop(b,&ops);
 assert(b->quiesced && !d->master && !b->live.active && !b->live.count && !b->live.tx_count);
 for(unsigned i=0;i<b->used;i++)assert(!b->pool[i]);
 for(unsigned i=0;i<48;i++)assert(!b->wpa.ptk[i]);
}
static uint8_t *next(WifiBoot *b) {uint8_t *p=b->rx+(b->consumed&511)*4096;memset(p,0,4096);return p;}
static void publish(WifiBoot *b,unsigned n){*(uint16_t *)(void *)b->status=((b->consumed+n)&511)|0x1000;}
static void finish_tx(WifiBoot *b,Device *d,unsigned status) {
 uint8_t *p=next(b);wpa_completion(p,hardware_write_pointer,0);p[8+40]=status;publish(b,1);
 WifiOps ops=live_ops(d);assert(!wifi_live_poll(b,&ops,1000));
}
static void arp(uint8_t *p,WifiBoot *b,unsigned pn) {
 unsigned len=24+8+8+28;memset(p,0,4096);p32(p,4+64+len);p[4]=0xc1;p[7]=0x80;
 uint8_t *desc=p+8,*f=desc+64;desc[0]=len;p32(desc+12,0x243);desc[42]=48;
 f[0]=8;f[1]=0x42;memcpy(f+4,b->mac,6);memcpy(f+10,b->target.bssid,6);memcpy(f+16,vector_ap,6);
 f[24]=pn;f[25]=pn>>8;f[27]=0x20;uint8_t snap[]={0xaa,0xaa,3,0,0,0,8,6};memcpy(f+32,snap,8);
 uint8_t *body=f+40;body[1]=1;body[2]=8;body[4]=6;body[5]=4;body[7]=2;
}
static void traffic(Device *d) {
 WifiBoot *b=d->b;WifiOps ops=live_ops(d);unsigned used=b->used,start=b->live.tx_head;
 uint8_t eth[1514]={0},out[1514];memcpy(eth+6,b->mac,6);eth[12]=8;eth[13]=6;unsigned n;
 for(unsigned i=1;i<=700;i++) {
  unsigned slot=b->live.tx_head&255;eth[20]=i;
  assert(wifi_live_tx(b,&ops,eth,42,100)==NIC_OK);
  uint8_t *cmd=b->live.tx_pages+slot*4096;assert(cmd[0]==0x1c && cmd[2]==slot && cmd[3]==1);
  assert((cmd[54]|(unsigned)cmd[55]<<8)==(((start+i-1)&4095)<<4));
  assert((u32(cmd+4)&0xffff)==60 && (u32(cmd+4)>>16)==1 && cmd[33]==0x41);
  assert(u64(b->management_tx+slot*256+4)==(uintptr_t)cmd && cmd[64+8+6]==eth[20]);
  eth[20]^=1;assert(cmd[64+8+6]!=eth[20]); /* native owned copy, never borrow guest buffer */
  finish_tx(b,d,1);assert(!b->live.tx_count && !b->live.tx_octets);
  uint8_t *p=next(b);arp(p,b,i);publish(b,1);assert(!wifi_live_poll(b,&ops,1000));
  assert(wifi_live_rx(b,out,10,&n)==NIC_TOO_SMALL && n==42 && b->live.count==1);
  assert(wifi_live_rx(b,out,sizeof(out),&n)==NIC_OK && n==42 && !b->live.count);
 }
 assert(b->consumed>1400 && b->used==used && b->live.tx_head==((start+700)&65535));
 NicInfo info;wifi_live_info(b,&info);assert(info.link && info.counters[0]==700 && info.counters[1]==700 && info.counters[3]==700 && info.counters[4]==700);
 /* Frame and byte quotas, cumulative SSN, failed TX, repeated completion. */
 for(unsigned i=0;i<64;i++)assert(wifi_live_tx(b,&ops,eth,42,100)==NIC_OK);
 assert(wifi_live_tx(b,&ops,eth,42,100)==NIC_WOULD_BLOCK);finish_tx(b,d,0x82);
 assert(!b->live.tx_count && b->live.info.counters[5]==1);finish_tx(b,d,1);assert(!b->live.tx_count);
 for(unsigned i=0;i<43;i++)assert(wifi_live_tx(b,&ops,eth,1514,100)==NIC_OK);
 assert(wifi_live_tx(b,&ops,eth,1514,100)==NIC_WOULD_BLOCK);finish_tx(b,d,1);
 assert(wifi_live_tx(b,&ops,eth,13,100)==NIC_DOWN);
 /* Bounded RX poll processes a 100-buffer batch in two calls, preserving the
  * unprocessed suffix and enforcing the host delivery queue quota. */
 for(unsigned i=0;i<100;i++)arp(b->rx+((b->consumed+i)&511)*4096,b,701+i);
 unsigned before=b->consumed;publish(b,100);assert(!wifi_live_poll(b,&ops,1000) && b->consumed==before+64);
 assert(!wifi_live_poll(b,&ops,1000) && b->consumed==before+100 && b->live.count==64);
 assert(b->live.info.counters[2]>=36);
 for(unsigned i=0;i<64;i++)assert(wifi_live_rx(b,out,sizeof(out),&n)==NIC_OK);
 assert(wifi_live_rx(b,out,sizeof(out),&n)==NIC_EMPTY);
 /* AX210+ consumes only one packet per buffer, regardless of tail contents. */
 uint8_t *p=next(b);wpa_completion(p,b->live.tx_head,0);p32(p+64,4093);p[68]=2;p[71]=0x80;
 publish(b,1);assert(!wifi_live_poll(b,&ops,1000));
}
static void counter_edges(WifiBoot *b,Device *d) {
 WifiOps ops=live_ops(d);uint8_t eth[42]={0},owners[256];memcpy(eth+6,b->mac,6);
 assert(!b->live.tx_count);b->live.tx_head=b->live.tx_tail=hardware_write_pointer=65530;b->live.sequence=4090;
 for(unsigned i=0;i<12;i++) {
  unsigned slot=(65530+i)&255;assert(wifi_live_tx(b,&ops,eth,sizeof(eth),0)==NIC_OK);
  const uint8_t *cmd=b->live.tx_pages+slot*4096;
  assert((cmd[54]|(unsigned)cmd[55]<<8)==(((4090+i)&4095)<<4));
 }
 assert(b->live.tx_head==6 && b->live.tx_tail==65530 && b->live.tx_count==12 && b->live.sequence==6);
 memcpy(owners,b->live.tx_owned,sizeof(owners));unsigned reclaimed=b->live.info.counters[4];
 for(unsigned i=0;i<2;i++) {
  /* A stale previous counter, or a future counter with the SAME LOW8 as a
   * pending slot. Neither can release a byte, even across 16-bit wrap. */
  uint8_t *p=next(b);wpa_completion(p,i?0xdead00fbU:65529,0);publish(b,1);
  assert(!wifi_live_poll(b,&ops,4999999) && b->live.active);
  assert(b->live.tx_head==6 && b->live.tx_tail==65530 && b->live.tx_count==12 && b->live.tx_octets==12*sizeof(eth));
  assert(!memcmp(owners,b->live.tx_owned,sizeof(owners)) && b->live.info.counters[4]==reclaimed);
 }
 uint8_t *p=next(b);wpa_completion(p,0,0);publish(b,1);assert(!wifi_live_poll(b,&ops,4999999));
 assert(b->live.tx_tail==0 && b->live.tx_count==6 && b->live.tx_octets==6*sizeof(eth));
 for(unsigned i=0;i<6;i++)assert(!b->live.tx_owned[250+i] && b->live.tx_owned[i]);
 p=next(b);wpa_completion(p,0,0);publish(b,1);assert(!wifi_live_poll(b,&ops,4999999) && b->live.tx_count==6);
 finish_tx(b,d,1);assert(!b->live.tx_count && b->live.tx_tail==6 && b->live.info.counters[4]==reclaimed+12);
}
/* Independent Linux v6.12 mvm/mac-ctxt.c + mvm.h policy oracle:
 * AX210/new-TX warns above8 since RX; disconnect requires19 total AND4
 * since RX. Values deliberately do not come from production constants. */
static void beacon_policy(void) {
 static const unsigned cases[][3]={{0,0,0},{8,8,0},{9,9,0},{10,10,0},
  {4,18,0},{0,19,0},{3,19,0},{4,19,1},{19,19,1},{3,100,0},{4,100,1}};
 for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
  WifiBoot b;Device d;start_live(&b,&d);WifiOps ops=live_ops(&d);
  uint8_t *p=next(&b);p32(p,24);p[4]=0xa2;p[7]=0x80;
  p32(p+8,0);p32(p+12,cases[i][0]);p32(p+16,cases[i][1]);
  p32(p+20,1234);p32(p+24,1200);publish(&b,1);
  const char *e=wifi_live_poll(&b,&ops,1000);
  if((e!=0)!=cases[i][2])fprintf(stderr,"beacon case %u since_rx=%u consecutive=%u: %s\n",i,cases[i][0],cases[i][1],e?e:"retained");
  assert((e!=0)==cases[i][2]);
  if(cases[i][2])assert(b.quiesced && !d.master && !b.live.active);
  else {
   assert(b.live.active && d.master && !b.quiesced);
   uint8_t eth[42]={0};memcpy(eth+6,b.mac,6);
   assert(wifi_live_tx(&b,&ops,eth,sizeof(eth),1001)==NIC_OK);finish_tx(&b,&d,1);
   assert(!b.live.tx_count);stop_live(&b,&d);
  }
 }
}
#ifndef WIFI_LIVE_MAIN
#define WIFI_LIVE_MAIN main
#endif
int WIFI_LIVE_MAIN(int argc,char **argv) {
 assert(!startup_tests(argc,argv));
 FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);blob=malloc(size);assert(blob && fread(blob,1,size,f)==(size_t)size);fclose(f);assert(!wifi_image_parse(blob,size,&image));
 f=fopen(argv[2],"rb");assert(f);fseek(f,0,SEEK_END);size=ftell(f);rewind(f);uint8_t *pnvm=malloc(size);assert(pnvm && fread(pnvm,1,size,f)==(size_t)size);fclose(f);
 image.pnvm=(WifiSection){pnvm,(unsigned)size};image.nvm_probe=image.scan_probe=image.auth_probe=image.assoc_probe=image.wpa_probe=image.live=1;
 memcpy(image.pmk,vector_pmk,32);memcpy(image.snonce,vector_snonce,32);image.prepare_ssid_size=3;memcpy(image.prepare_ssid,"abc",3);
 auth_fault=AUTH_OK;assoc_fault=ASSOC_OK;scan_fault=SCAN_OK;wpa_fault=WPA_OK;key_fault=0;beacon_group=2;fault_kind=update_fault=0;expected_basic_ofdm=0x15;
 beacon_policy();
 WifiBoot b;Device d;start_live(&b,&d);traffic(&d);counter_edges(&b,&d);stop_live(&b,&d);
 for(unsigned fault=0;fault<12;fault++) {
  start_live(&b,&d);WifiOps ops=live_ops(&d);uint8_t *p=next(&b);wpa_completion(p,b.live.tx_tail,0);
  switch(fault) {
   case 0:p32(p,4093);break;
   case 1:p[8+36]=2;break;
   case 2: /* An impossible completion cannot reclaim; the NIC stays usable. */
    p32(p+8+44,b.live.tx_tail+1);publish(&b,1);
    assert(!wifi_live_poll(&b,&ops,1000) && b.live.active && !b.live.tx_count && b.live.ignored_ssn==1 && b.live.info.counters[8]==1 && !b.live.info.counters[7]);
    stop_live(&b,&d);continue;
   case 3:p[4]=2;break;
   case 4:d.reg[8/4]=1U<<25;break;
   case 5:d.fault=RFKILL;d.reg[0x24/4]&=~0x08000000U;break;
   case 6:p32(p,8);p[4]=0xc9;p[7]=0x80;p[8]=0;break;
   case 7:p32(p,24);p[4]=0xa2;p[7]=0x80;p32(p+8,0);p32(p+12,4);p32(p+16,19);break;
   case 8:ap_eapol(p,&b,1);p[8+64+24+8+16]=9;break;
   case 9:p32(p,24);p[4]=0xa2;p[7]=0x80;p32(p+8,7);break;
   case 10: {
    uint8_t eth[42]={0};memcpy(eth+6,b.mac,6);assert(wifi_live_tx(&b,&ops,eth,42,0)==NIC_OK);
    assert(wifi_live_poll(&b,&ops,5000000));assert(b.quiesced && !d.master);continue;
   }
   case 11: {
    uint8_t eth[42]={0};memcpy(eth+6,b.mac,6);assert(wifi_live_tx(&b,&ops,eth,42,0)==NIC_OK);
    p32(p+8+44,b.live.tx_tail+257);publish(&b,1);
    assert(!wifi_live_poll(&b,&ops,4999999) && b.live.active && b.live.tx_count==1 && b.live.ignored_ssn==1 && b.live.info.counters[8]==1 && !b.live.info.counters[7]);
    assert(wifi_live_poll(&b,&ops,5000000) && b.quiesced && !d.master);continue;
   }
  }
  publish(&b,1);const char *failure=wifi_live_poll(&b,&ops,1000);
  if(!failure || !b.quiesced || d.master || b.live.active)fprintf(stderr,"fault %u: %s stopped=%u master=%u active=%u\n",fault,failure?failure:"none",b.quiesced,d.master,b.live.active);
  assert(failure && b.quiesced && !d.master && !b.live.active);
 }
 /* A failed hardware stop keeps DMA/key contents resident rather than freeing
  * or wiping memory while the endpoint might still own it. */
 start_live(&b,&d);d.fault=STOP_TIMEOUT;WifiOps ops=live_ops(&d);b.pool[0]=0xab;wifi_firmware_stop(&b,&ops);
 assert(!b.quiesced && b.pool[0]==0xab && !b.live.active && !d.master);
 d.fault=NVM_OPAQUE_STATUS;stop_live(&b,&d);
 /* The historical zero-SPA diagnostic cannot gate a production link.
  * This fake AP completes WPA2 but supplies no diagnostic ARP reply. */
 image.data_probe=1;image.data_target[0]=192;image.data_target[1]=0;
 image.data_target[2]=2;image.data_target[3]=1;
 start_live(&b,&d);assert(!b.data.enabled && !b.data.sent);stop_live(&b,&d);
 free(blob);free(pnvm);puts("AX211_LIVE_HOST_OK independent 16-bit hardware / 8-bit DMA / 12-bit MAC wrap; stale/ahead/partial SSNs, 700 TX/1400 RX, quotas/copy/watchdog/fault/stop; RF untested");
 return 0;
}
