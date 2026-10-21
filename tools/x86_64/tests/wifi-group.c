/* Independent OpenBSD Michael oracle fixtures and GTK RX framing/atomicity. */
#define main unicast_tests
#include "wifi-ether.c"
#undef main
#include "fixtures/wifi-michael-vectors.h"
static void michael_tests(void) {
 uint8_t key[8],da[6],sa[6]={6,7,8,9,10,11},data[1500],digest[8];memset(da,255,6);
 for(unsigned i=0;i<8;i++)key[i]=255-i*23;
 for(unsigned i=0;i<1500;i++)data[i]=i*37+13;
 for(unsigned i=0;i<68;i++) {
  unsigned n=i<66?i:i==66?1499:1500;wifi_michael(key,da,sa,n%8,data,n,digest);
  assert(!memcmp(digest,michael_vectors[i],8));
 }
}
static unsigned group_packet(WifiBoot *b,uint8_t *p,unsigned tkip,unsigned rada,unsigned pad,unsigned trailer) {
 *b=fixture();b->live.active=1;b->target.group_cipher=tkip?4:16;b->wpa.gtk_bytes=tkip?32:16;b->wpa.gtk_id=1;
 for(unsigned i=0;i<32;i++)b->wpa.gtk[i]=255-i*23;
 b->rekey.keys[1]=(WifiGroupKey){.valid=1,.bytes=tkip?32:16};memcpy(b->rekey.keys[1].key,b->wpa.gtk,32);
 unsigned n=packet(p,1,pad,(!rada&&tkip?12:0)+trailer,0x806,28);uint8_t *f=p+64,*iv=f+26,*llc=iv+8+pad;
 memset(f+4,255,6);f[24]=3;iv[0]=tkip?0:1;iv[2]=tkip?1:0;iv[3]=0x60;
 p32(p+12,tkip?0x363:0x243);p[2]=(trailer/2)<<4;
 if(tkip&&!rada)wifi_michael(b->wpa.gtk+16,f+4,f+16,3,llc,36,llc+36);
 return n;
}
int main(void) {
 unicast_tests();michael_tests();uint8_t p[4096],out[1514];unsigned length,cases=0;
 for(unsigned tkip=0;tkip<2;tkip++)for(unsigned rada=0;rada<2;rada++)for(unsigned pad=0;pad<=2;pad+=2)for(unsigned trailer=0;trailer<=4;trailer+=4) {
  WifiBoot b;unsigned n=group_packet(&b,p,tkip,rada,pad,trailer);
  assert(wifi_data_decode_flags(&b,p,n,out,&length,rada?1U<<26:0) && length==42 && out[0]==255 && b.rekey.keys[1].pn[3]==1 && !b.data.rx_pn[3]);
  assert(!wifi_data_decode_flags(&b,p,n,out,&length,rada?1U<<26:0) && !length && b.data.replays==1);cases+=2;
  for(unsigned fault=0;fault<10;fault++) {
   n=group_packet(&b,p,tkip,rada,pad,trailer);uint8_t *f=p+64,*iv=f+26;
   switch(fault) {
    case 0:b.live.active=0;break;case 1:b.rekey.keys[1].bytes=0;break;case 2:iv[3]=0x20;break;
    case 3:b.rekey.keys[1].pn[3]=1;break;case 4:p[12]&=~64;break;case 5:p32(p+12,3);break;
    case 6:p[12]&=~32;if(!tkip)p[12]&=~64;break;
    case 7:f[1]&=~64;break;case 8:f[10]^=2;break;
    case 9:if(tkip&&!rada)f[26+8+pad+8+28]^=1;else p[12]&=~64;break;
   }
   memset(out,0x55,sizeof(out));assert(!wifi_data_decode_flags(&b,p,n,out,&length,rada?1U<<26:0) && !length && out[0]==0x55);
   assert(b.rekey.keys[1].pn[3]==(fault==3?1U:0U));++cases;
  }
 }
 puts("AX211_GROUP_HOST_OK 68 independent OpenBSD Michael oracle vectors; GTK CCMP/TKIP RADA/pad/trailer/MIC/replay/key/atomicity guards; RF untested");
 assert(cases==192);return 0;
}
