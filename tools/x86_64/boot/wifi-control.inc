/* Descriptor ABI1: version,size,interface,action,reserved,reserved,
 * state,outcome,generationLo/Hi,attemptsLo/Hi,error[64]. Validated before
 * any radio side effect. No C16 address is retained across a restart. */
#include "wifi-restart.h"
static void wifi_control_device(Context *c) {
 U p=((U)c->desc_bank<<16)|c->desc_offset;size_t at;
 if(!network_span(p,2,&at)){c->device_status=4;return;}
 uint16_t *d=c->cells+at;
 if(d[0]!=1){c->device_status=2;return;}
 if(d[1]!=WIFI_CONTROL_CELLS){c->device_status=3;return;}
 if(!network_span(p,WIFI_CONTROL_CELLS,&at)){c->device_status=4;return;}
 if(d[2] || d[3]>WIFI_CONTROL_RECONNECT || d[4] || d[5]){c->device_status=5;return;}
 WifiRestartReply r;wifi_control(d[3],&r);
 /* Startup is native and cannot switch processes or invalidate the descriptor. */
 dirty(c,at,WIFI_CONTROL_CELLS);d[6]=r.state;d[7]=r.outcome;
 d[8]=r.generation;d[9]=r.generation>>16;d[10]=r.attempts;d[11]=r.attempts>>16;
 unsigned i=0;for(;r.error && r.error[i] && i<WIFI_CONTROL_TEXT-1;i++)d[12+i]=(uint8_t)r.error[i];
 for(;i<WIFI_CONTROL_TEXT;i++)d[12+i]=0;
 c->device_status=0;
}
