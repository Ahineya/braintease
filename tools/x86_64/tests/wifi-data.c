/* Full production startup/WPA/protected ARP state machine, fake firmware DMA.
 * Verify TX encrypt flag, wire addresses/body/TFH, crypto/reply/error/stop gates. */
#define WIFI_WPA_TRANSPORT_MAIN wifi_wpa_transport_tests
#include "wifi-wpa-transport.c"
enum {DATA_OK,DATA_RX_FIRST,DATA_QOS_PAD,DATA_MIC_ABSENT,DATA_DEC_BIT_ABSENT,
 DATA_NO_TX,DATA_TX_FAILED,DATA_BAD_QUEUE,DATA_NO_RX,DATA_WRONG_IP,DATA_WRONG_TARGET,
 DATA_WRONG_SOURCE,DATA_CLEAR_RX,DATA_BAD_CIPHER,DATA_DUPLICATE,DATA_BAD_THEN_GOOD,DATA_ZERO_PN,DATA_LAST};
static unsigned data_fault;
static void data_response(uint8_t *p,WifiBoot *b) {
 memset(p,0,4096);unsigned qos=data_fault==DATA_QOS_PAD,h=qos?26:24,pad=qos?2:0,len=h+8+pad+8+28+8;
 p32(p,4+64+len);p[4]=0xc1;p[7]=0x80;uint8_t *d=p+8,*f=d+64;
 d[0]=len;d[3]=pad?0x20:0;p32(d+12,data_fault==DATA_DEC_BIT_ABSENT?0x243:0xa43);d[42]=48;
 f[0]=qos?0x88:8;f[1]=data_fault==DATA_CLEAR_RX?2:0x42;
 memcpy(f+4,b->mac,6);memcpy(f+10,b->target.bssid,6);static const uint8_t peer[6]={6,7,8,9,10,11};memcpy(f+16,peer,6);
 uint8_t *iv=f+h;iv[0]=1;iv[3]=0x20;uint8_t *llc=iv+8+pad,*arp=llc+8;
 const uint8_t snap[]={0xaa,0xaa,3,0,0,0,8,6};memcpy(llc,snap,8);
 arp[1]=1;arp[2]=8;arp[4]=6;arp[5]=4;arp[7]=2;
 memcpy(arp+8,peer,6);memcpy(arp+14,b->data.target,4);memcpy(arp+18,b->mac,6);
 if(data_fault==DATA_MIC_ABSENT)d[12]&=~64;
 if(data_fault==DATA_BAD_CIPHER)d[13]=(d[13]&~7)|3;
 if(data_fault==DATA_ZERO_PN)iv[0]=0;
 if(data_fault==DATA_WRONG_IP)arp[14]^=1;
 if(data_fault==DATA_WRONG_TARGET)arp[18]^=2;
 if(data_fault==DATA_WRONG_SOURCE)arp[8]^=2;
}
static void data_write(void *opaque,unsigned reg,uint32_t v) {
 Device *d=opaque;WifiBoot *b=d->b;
 if(reg!=0x460 || !b->data.sent){wpa_write(opaque,reg,v);return;}
 assert(v==0x10005 && b->wpa.complete && b->wpa.key_installed==2 && d->master);
 unsigned index=(v&65535)-1;uint8_t *desc=b->management_tx+index*256,*cmd=(void *)(uintptr_t)u64(desc+4),*f=cmd+32,*body=cmd+64;
 assert(cmd[0]==0x1c && cmd[2]==index && cmd[3]==1 && u32(cmd+4)==0x1003c);
 assert(u32(cmd+8)==0xc00 && u32(cmd+20)==0x4100 && f[0]==8 && f[1]==0x41);
 assert(!memcmp(f+4,vector_ap,6) && !memcmp(f+10,vector_own,6) && f[22]==index*16);
 for(unsigned i=0;i<6;i++)assert(f[16+i]==255);
 const uint8_t snap[]={0xaa,0xaa,3,0,0,0,8,6};assert(!memcmp(body,snap,8));
 uint8_t expected[28]={0};expected[1]=1;expected[2]=8;expected[4]=6;expected[5]=4;expected[7]=1;
 memcpy(expected+8,vector_own,6);memcpy(expected+24,b->data.target,4);assert(!memcmp(body+8,expected,28));
 assert((u32(desc)&65535)==3 && u32(desc)>>16==20 && (u32(desc+12)&65535)==36 && (u32(desc+22)&65535)==36);
 assert(u64(desc+14)==(uintptr_t)cmd+20 && u64(desc+24)==(uintptr_t)body);
 assert((b->management_bc[index*2]|(unsigned)b->management_bc[index*2+1]<<8)==60);
 unsigned n=b->consumed;
 if(data_fault==DATA_RX_FIRST)data_response(b->rx+n++*4096,b);
 if(data_fault!=DATA_NO_TX) {
  uint8_t *p=b->rx+n++*4096;wpa_completion(p,v&65535,0);
  if(data_fault==DATA_TX_FAILED)p[8+40]=0x82;
  if(data_fault==DATA_BAD_QUEUE)p[8+36]=2;
 }
 if(data_fault!=DATA_NO_RX && data_fault!=DATA_RX_FIRST) {
  if(data_fault==DATA_BAD_THEN_GOOD){data_response(b->rx+n*4096,b);b->rx[n++*4096+8+12]&=~64;}
  data_response(b->rx+n++*4096,b);
  if(data_fault==DATA_DUPLICATE)data_response(b->rx+n++*4096,b);
 }
 *(uint16_t *)(void *)b->status=n;
}
int main(int argc,char **argv) {
 assert(!wifi_wpa_transport_tests(argc,argv));
 FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);blob=malloc(size);assert(blob && fread(blob,1,size,f)==(size_t)size);fclose(f);assert(!wifi_image_parse(blob,size,&image));
 f=fopen(argv[2],"rb");assert(f);fseek(f,0,SEEK_END);size=ftell(f);rewind(f);uint8_t *pnvm=malloc(size);assert(pnvm && fread(pnvm,1,size,f)==(size_t)size);fclose(f);
 image.pnvm=(WifiSection){pnvm,(unsigned)size};image.nvm_probe=image.scan_probe=image.auth_probe=image.assoc_probe=image.wpa_probe=image.data_probe=1;
 memcpy(image.pmk,vector_pmk,32);memcpy(image.snonce,vector_snonce,32);image.prepare_ssid_size=3;memcpy(image.prepare_ssid,"abc",3);
 const uint8_t target[4]={192,0,2,1};memcpy(image.data_target,target,4);
 auth_fault=AUTH_OK;assoc_fault=ASSOC_OK;scan_fault=SCAN_OK;wpa_fault=WPA_OK;key_fault=0;beacon_group=2;fault_kind=update_fault=0;expected_basic_ofdm=0x15;
 for(data_fault=0;data_fault<DATA_LAST;data_fault++) {
  WifiBoot b={.pool=pool,.capacity=sizeof(pool)};Device d={.b=&b,.fault=NVM_OPAQUE_STATUS};
  WifiOps ops={&d,wpa_read,data_write,write_byte,delay,mastering,prepare_log};
  const char *error=wifi_firmware_start(&b,&image,&ops,0x441);
  unsigned success=data_fault<=DATA_QOS_PAD || data_fault==DATA_DEC_BIT_ABSENT || data_fault==DATA_DUPLICATE || data_fault==DATA_BAD_THEN_GOOD;
  if((error==0)!=success)fprintf(stderr,"data %u: %s\n",data_fault,error?error:"unexpected success");
  assert((error==0)==success && b.data.complete==success && b.quiesced && !d.master);
  if(success)assert(b.data.reply && b.data.tx_status==1 && b.data.rx_packets==1 && b.data.last_pn==1);
  if(data_fault==DATA_DUPLICATE)assert(b.data.replays==1 && b.data.drops==1);
  if(data_fault==DATA_BAD_THEN_GOOD)assert(!b.data.replays && b.data.drops==1);
  for(unsigned i=0;i<b.used;i++)assert(!pool[i]);
 }
 for(unsigned n=0;n<2;n++) {
  WifiImage bad=image;if(n)bad.data_target[0]=224;else bad.wpa_probe=0;
  WifiBoot b={.pool=pool,.capacity=sizeof(pool)};Device d={.b=&b};WifiOps ops={&d,wpa_read,data_write,write_byte,delay,mastering,prepare_log};
  assert(wifi_firmware_start(&b,&bad,&ops,0x441) && !d.writes && !b.attempted);
 }
 free(blob);free(pnvm);printf("AX211_DATA_HOST_OK %u full protected-ARP/TX/RX/failure/profile/stop cases; RF untested\n",DATA_LAST+2);return 0;
}
