/* Malformed control requests must never touch radio state. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define BANK 64000U
#define CELLS (256U*BANK)
typedef uint64_t U;
typedef struct {uint16_t cells[CELLS],desc_bank,desc_offset,device_status;} Context;
static Context c;static unsigned calls,marks;
static int network_span(U p,U n,size_t *at){U bank=p>>16,off=p&65535;if(bank>=256||off>=BANK)return 0;*at=bank*BANK+off;return *at>=32 && n<=CELLS-*at;}
static void dirty(Context *ctx,size_t at,size_t n){assert(ctx==&c && at+n<=CELLS);++marks;}
#include "../boot/wifi-restart.h"
void wifi_control(unsigned action,WifiRestartReply *r){++calls;assert(action<=2);*r=(WifiRestartReply){.state=WIFI_STATE_UP,.generation=0x12345678,.attempts=0xabcdef01,.error="test reason"};}
#include "../boot/wifi-control.inc"
static uint16_t *desc(void){c.desc_bank=2;c.desc_offset=63980;size_t at=2*BANK+63980;memset(c.cells+at,0,WIFI_CONTROL_CELLS*2);uint16_t *d=c.cells+at;d[0]=1;d[1]=WIFI_CONTROL_CELLS;return d;}
static void reject(unsigned code){unsigned before=calls,was=marks;wifi_control_device(&c);assert(c.device_status==code && calls==before && marks==was);}
int main(void){uint16_t *d;
 for(unsigned action=0;action<=2;action++){d=desc();d[3]=action;wifi_control_device(&c);assert(!c.device_status && d[6]==2 && d[8]==0x5678 && d[9]==0x1234 && d[10]==0xef01 && d[11]==0xabcd && d[12]=='t' && !d[23]);}
 d=desc();d[0]=2;reject(2);d=desc();d[1]--;reject(3);
 for(unsigned field=2;field<=5;field++){d=desc();d[field]=field==3?3:1;reject(5);}
 desc();c.desc_bank=256;reject(4);desc();c.desc_offset=BANK;reject(4);desc();c.desc_bank=255;c.desc_offset=BANK-75;c.cells[CELLS-75]=1;c.cells[CELLS-74]=WIFI_CONTROL_CELLS;reject(4);
 desc();c.desc_bank=255;c.desc_offset=BANK-1;reject(4);desc();c.desc_bank=0;c.desc_offset=0;reject(4);
 puts("WiFi control mailbox guards passed: cross-bank, malformed, reserved, bounds, no radio side effects");
}
