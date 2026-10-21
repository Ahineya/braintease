/* Public raw-frame fixtures: test the production native converter, not crypto
 * by assuming that fake status bits prove RF. Live hardware remains separate. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../boot/wifi.h"
static const uint8_t own[6]={2,3,4,5,6,7},ap[6]={4,5,6,7,8,9},peer[6]={6,7,8,9,10,11};
static void p32(uint8_t *p,unsigned n){for(unsigned i=0;i<4;i++)p[i]=n>>(8*i);}
static WifiBoot fixture(void) {
 WifiBoot b={0};memcpy(b.mac,own,6);memcpy(b.target.bssid,ap,6);b.target.channel=48;
 b.wpa.complete=1;b.wpa.key_installed=2;return b;
}
static unsigned packet(uint8_t *p,unsigned qos,unsigned pad,unsigned trailer,unsigned type,unsigned payload) {
 memset(p,0,4096);unsigned h=qos?26:24,n=h+8+pad+8+payload+trailer;p[0]=n;p[1]=n>>8;p[3]=pad?0x20:0;
 p32(p+12,0xa43);p[42]=48;uint8_t *f=p+64;f[0]=qos?0x88:8;f[1]=0x42;
 memcpy(f+4,own,6);memcpy(f+10,ap,6);memcpy(f+16,peer,6);uint8_t *iv=f+h;iv[0]=1;iv[3]=0x20;
 uint8_t *llc=iv+8+pad;llc[0]=llc[1]=0xaa;llc[2]=3;llc[6]=type>>8;llc[7]=type;
 uint8_t *body=llc+8;
 if(type==0x806){body[1]=1;body[2]=8;body[4]=6;body[5]=4;body[7]=2;}
 if(type==0x800){body[0]=0x45;body[2]=payload>>8;body[3]=payload;}
 if(type==0x86dd){body[0]=0x60;body[4]=(payload-40)>>8;body[5]=payload-40;}
 return 64+n;
}
int main(void) {
 uint8_t p[4096],out[WIFI_ETH_FRAME],before[WIFI_ETH_FRAME];unsigned length,cases=0;
 for(unsigned qos=0;qos<2;qos++)for(unsigned pad=0;pad<=2;pad+=2)for(unsigned tail=0;tail<=8;tail+=8) {
  WifiBoot b=fixture();unsigned n=packet(p,qos,pad,tail,0x806,28);
  assert(wifi_data_decode(&b,p,n,out,&length) && length==42 && b.data.last_pn==1);
  assert(!memcmp(out,own,6) && !memcmp(out+6,peer,6) && out[12]==8 && out[13]==6);
  assert(!wifi_data_decode(&b,p,n,out,&length) && !length && b.data.replays==1);cases+=2;
  for(unsigned cut=0;cut<n;cut++) {b=fixture();memset(out,0x55,sizeof(out));memcpy(before,out,sizeof(out));
   assert(!wifi_data_decode(&b,p,cut,out,&length) && !length && !memcmp(out,before,sizeof(out)) && !b.data.rx_pn[0]);++cases;
  }
 }
 for(unsigned fault=0;fault<22;fault++) {
  WifiBoot b=fixture();unsigned n=packet(p,1,2,8,0x806,28);uint8_t *f=p+64,*iv=f+26,*llc=iv+10;
  unsigned good=0;
  switch(fault) {
   case 0:p[12]&=~1;break;case 1:p[12]&=~2;break;case 2:p[12]&=~64;break;
   case 3:p[13]=(p[13]&~7)|3;break;case 4:f[1]&=~64;break;case 5:f[1]|=4;break;
   case 6:f[4]^=2;break;case 7:f[10]^=2;break;case 8:p[42]=44;break;
   case 9:f[22]|=1;break;case 10:iv[3]=0;break;case 11:iv[3]|=64;break;
   case 12:iv[0]=0;break;case 13:f[24]=128;break;case 14:p[3]|=64;break;
   case 15:f[24]=8;break;case 16:llc[0]=0;break;case 17:llc[8+4]=5;break;
   case 18:b.wpa.complete=0;break;case 19:b.wpa.key_installed=1;break;
   case 20:p[13]&=~8;good=1;break; /* modern Linux does not gate DECRYPTED */
   case 21:p32(p+12,0xfff00a43);good=1;break; /* unknown status bits opaque */
  }
  memset(out,0x55,sizeof(out));memcpy(before,out,sizeof(out));
  assert(wifi_data_decode(&b,p,n,out,&length)==good);
  if(!good)assert(!length && !memcmp(out,before,sizeof(out)) && !b.data.rx_pn[0]);++cases;
 }
 for(unsigned type=0;type<2;type++) {
  WifiBoot b=fixture();unsigned n=packet(p,0,0,8,type?0x86dd:0x800,1500);
  assert(wifi_data_decode(&b,p,n,out,&length) && length==1514);++cases;
 }
 WifiBoot b=fixture();unsigned n=packet(p,1,0,0,0x806,28);p[64+24]=7;
 assert(wifi_data_decode(&b,p,n,out,&length) && b.data.rx_pn[7]==1 && !b.data.rx_pn[0]);++cases;
 p[64+24]=0;assert(wifi_data_decode(&b,p,n,out,&length) && b.data.rx_pn[0]==1);++cases;
 uint8_t enc[1600];memcpy(out+6,own,6);n=wifi_data_encode(&b,out,42,enc,sizeof(enc));assert(n==60 && enc[0]==8 && enc[1]==0x41);
 assert(!memcmp(enc+4,ap,6) && !memcmp(enc+10,own,6) && !memcmp(enc+16,own,6));++cases;
 assert(!wifi_data_encode(&b,out,42,enc,59));out[6]^=2;assert(!wifi_data_encode(&b,out,42,enc,sizeof(enc)));cases+=2;
 printf("AX211_ETHER_HOST_OK %u framing/crypto-status/PN/pad/length/atomicity cases; RF untested\n",cases);
 return 0;
}
