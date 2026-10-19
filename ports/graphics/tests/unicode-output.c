/* Deterministic terminal font acceptance fixture, on a real guest PTY. */
#include <unistd.h>
int main(void) {
    static const char text[]={27,'[','H','A',0xd0,0x96,0xce,0x93,'e',0xcc,0x81,0xe4,0xb8,0xad,0xf0,0x9f,0x99,0x82,'Z'};
    static char tick[]={27,'[','3',';','1','H','0'};
    unsigned i;
    write(1,text,sizeof(text));
    for(i=0;i<4;i++){sleep(1);tick[6]='0'+i;write(1,tick,sizeof(tick));}
    return 0;
}
