/* Explicit synchronous development control. Never runs from a NIC poll.
 * No automatic retry, allocation, C16 callback or DMA/key ownership here. */
#include "wifi-restart.h"
static unsigned state(const WifiRestartState *s) {
 if(s->attempted && !s->active && !s->quiesced)return WIFI_STATE_BLOCKED;
 return !s->present?WIFI_STATE_ABSENT:s->active?WIFI_STATE_UP:WIFI_STATE_DOWN;
}
static void count(uint32_t *n){if(*n!=UINT32_MAX)++*n;}
void wifi_restart(WifiRestart *r,const WifiRestartOps *o,unsigned action,WifiRestartReply *out) {
 WifiRestartState s;o->read(o->opaque,&s);unsigned result=WIFI_RESULT_OK;
 if(action==WIFI_CONTROL_STATUS)goto done;
 if(action>WIFI_CONTROL_RECONNECT){result=WIFI_RESULT_START_FAILED;goto done;}
 if(r->busy){result=WIFI_RESULT_BUSY;goto done;}
 r->busy=1;
 if(s.active || (s.attempted && !s.quiesced)) {
  o->stop(o->opaque);o->read(o->opaque,&s);
  if(!s.quiesced || s.active){r->error="device stop unconfirmed; reconnect blocked";result=WIFI_RESULT_BLOCKED;goto finished;}
 }
 if(action==WIFI_CONTROL_DOWN){r->error=0;goto finished;}
 if(action!=WIFI_CONTROL_RECONNECT){result=WIFI_RESULT_START_FAILED;r->error="invalid reconnect action";goto finished;}
 if(!s.present){result=WIFI_RESULT_ABSENT;r->error="AX211 is unavailable";goto finished;}
 count(&r->attempts);r->error=o->start(o->opaque);o->read(o->opaque,&s);
 if(r->error || !s.active) {
  if(!r->error)r->error="WiFi startup did not produce a link";
  result=state(&s)==WIFI_STATE_BLOCKED?WIFI_RESULT_BLOCKED:WIFI_RESULT_START_FAILED;
 } else count(&r->generation);
finished:r->busy=0;
done:*out=(WifiRestartReply){state(&s),result,r->attempts,r->generation,r->error};
}
