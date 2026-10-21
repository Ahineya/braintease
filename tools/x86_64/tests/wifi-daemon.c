/* Execute the actual daemon loop with deterministic radio/time/process ops.
 * No hardware, real child processes, network changes or on-disk pause files. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>
#include <signal.h>
#define WIFI_CLIENT_TEST 1
#include "../userland/wifi-client.h"
static unsigned fake_sleep(unsigned);
static int fake_access(const char *,int);
static int fake_clock(clockid_t,struct timespec *);
static uid_t fake_euid(void);
static pid_t fake_fork(void);
static pid_t fake_wait(pid_t,int *,int);
#define sleep fake_sleep
#define access fake_access
#define clock_gettime fake_clock
#define geteuid fake_euid
#define fork fake_fork
#define waitpid fake_wait
#define main daemon_main
#include "../userland/wifid.c"
#undef main
static uint32_t ms,attempts_at[10];
static unsigned scenario,link_state,attempts,generation,hooks,downs;
static uid_t fake_euid(void){return 0;}
static int fake_access(const char *p,int mode){(void)p;(void)mode;return !scenario&&ms>=10000&&ms<15000?0:-1;}
static int fake_clock(clockid_t id,struct timespec *t){(void)id;t->tv_sec=ms/1000;t->tv_nsec=(ms%1000)*1000000;return 0;}
static pid_t fake_fork(void){hooks++;return 42;}
static pid_t fake_wait(pid_t p,int *s,int flags){assert(p==42&&!flags);*s=0;return p;}
static unsigned fake_sleep(unsigned seconds){ms+=seconds*1000;if(!scenario&&ms==8000)link_state=WIFI_STATE_DOWN;if(ms>=20000)raise(SIGTERM);return 0;}
unsigned wifi_request(unsigned op,WifiSnapshot *r){
    if(scenario==2)return 7;
    if(op==WIFI_CONTROL_RECONNECT){
        assert(!scenario && attempts<10);attempts_at[attempts++]=ms;
        if(attempts>2){link_state=WIFI_STATE_UP;generation++;}
    }
    if(op==WIFI_CONTROL_DOWN){downs++;if(scenario!=1)link_state=WIFI_STATE_DOWN;}
    *r=(WifiSnapshot){0};r->state=link_state;r->generation=generation;r->attempts=attempts;
    if(op==WIFI_CONTROL_RECONNECT && attempts<=2)r->outcome=WIFI_RESULT_START_FAILED;
    return 0;
}
int main(void){
    link_state=WIFI_STATE_DOWN;assert(!daemon_main());
    assert(attempts==5&&hooks==3&&downs==2);
    const uint32_t expected[]={0,2000,6000,8000,15000};
    for(unsigned i=0;i<5;i++)assert(attempts_at[i]==expected[i]);
    scenario=1;ms=attempts=hooks=downs=stopping=0;link_state=WIFI_STATE_BLOCKED;
    assert(!daemon_main()&&!attempts&&!hooks&&downs==1);
    scenario=2;stopping=0;assert(daemon_main()==1);
    puts("WiFi actual daemon loop passed: failed boot retries, rekey stop/recovery, hooks, pause/resume, unsafe-stop refusal, TERM, unsupported control");
}
