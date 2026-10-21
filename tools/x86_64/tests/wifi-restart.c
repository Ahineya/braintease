#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../boot/wifi-restart.h"
typedef struct {WifiRestartState s;unsigned stops,starts,stop_bad,start_bad,nonce;WifiRestart *controller;WifiRestartOps *ops;} Mock;
static void read_state(void *v,WifiRestartState *s){*s=((Mock*)v)->s;}
static void stop(void *v){Mock *m=v;++m->stops;m->s.active=0;if(!m->stop_bad)m->s.quiesced=1;}
static const char *start(void *v){Mock *m=v;assert(!m->s.active && (!m->s.attempted || m->s.quiesced));++m->starts;++m->nonce;m->s.attempted=1;m->s.quiesced=0;m->s.active=!m->start_bad;if(m->start_bad){m->s.quiesced=m->start_bad==1;return "authentication rejected";}return 0;}
int main(void){
 WifiRestart c={0};Mock m={.s={1,1,1,0}};WifiRestartOps o={&m,read_state,stop,start};WifiRestartReply r;
 wifi_restart(&c,&o,0,&r);assert(r.state==WIFI_STATE_UP && !m.stops && !m.starts);
 wifi_restart(&c,&o,99,&r);assert(r.outcome && !m.stops && !m.starts);
 wifi_restart(&c,&o,1,&r);assert(!r.outcome && r.state==WIFI_STATE_DOWN && m.stops==1);
 for(unsigned i=0;i<100;i++){wifi_restart(&c,&o,2,&r);assert(!r.outcome && r.state==WIFI_STATE_UP && r.generation==i+1 && m.starts==i+1 && m.nonce==i+1);}
 m.stop_bad=1;unsigned before=m.starts;wifi_restart(&c,&o,2,&r);assert(r.outcome==WIFI_RESULT_BLOCKED && r.state==WIFI_STATE_BLOCKED && m.starts==before && strstr(r.error,"stop unconfirmed"));
 wifi_restart(&c,&o,2,&r);assert(r.outcome==WIFI_RESULT_BLOCKED && m.starts==before);
 m.stop_bad=0;m.start_bad=1;wifi_restart(&c,&o,2,&r);assert(r.outcome==WIFI_RESULT_START_FAILED && r.state==WIFI_STATE_DOWN && r.generation==100 && strstr(r.error,"authentication rejected"));
 m.start_bad=2;wifi_restart(&c,&o,2,&r);assert(r.outcome==WIFI_RESULT_BLOCKED && r.state==WIFI_STATE_BLOCKED && r.generation==100);
 m.stop_bad=1;before=m.starts;wifi_restart(&c,&o,2,&r);assert(r.outcome==WIFI_RESULT_BLOCKED && m.starts==before);
 c.busy=1;before=m.stops;wifi_restart(&c,&o,2,&r);assert(r.outcome==WIFI_RESULT_BUSY && m.stops==before);c.busy=0;
 m.s=(WifiRestartState){0,0,0,0};before=m.starts;wifi_restart(&c,&o,2,&r);assert(r.outcome==WIFI_RESULT_ABSENT && m.starts==before);
 c.attempts=c.generation=UINT32_MAX;m.start_bad=m.stop_bad=0;m.s=(WifiRestartState){1,0,1,1};wifi_restart(&c,&o,2,&r);assert(!r.outcome && r.attempts==UINT32_MAX && r.generation==UINT32_MAX);
 puts("WiFi restart checks passed: query,100 stopped/fresh-start cycles, unsafe-stop refusal, failed auth, absent,busy,invalid,saturation");
}
