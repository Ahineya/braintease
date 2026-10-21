/* Positive resolver caching through actual guest sockets and monotonic time. */
#include <netutil.h>
#include <netdb.h>
#include <string.h>
static unsigned checks, failures;
static void check(int yes) { ++checks; if(!yes) ++failures; }
static void lookup(const char *name,const char *canonical) {
    struct hostent *h=gethostbyname(name);
    check(h && !strcmp(h->h_name,canonical) && h->h_addr_list[0] && !h->h_addr_list[1]);
}
int main(void) {
    FILE *f; unsigned i;
    lookup("cache.test","cache.test"); lookup("CACHE.TEST.","cache.test");
    lookup("zero.test","zero.test"); lookup("zero.test","zero.test");
    lookup("alias.test","target.test"); lookup("alias.test","target.test");
    /* Alias TTL is one second, while the terminal A record lives 120 seconds. */
    for(i=0;i<15;i++) nu_wait();
    lookup("alias.test","target.test");
    lookup("cache.test","cache.test");
    f=fopen("/etc/hosts","w");check(f!=NULL);
    if(f) { fputs("192.168.64.1 override.test cache.test\n",f);fclose(f); }
    lookup("cache.test","override.test");
    f=fopen("/etc/hosts","w");if(f)fclose(f);
    lookup("cache.test","cache.test");
    /* A changed resolver list invalidates an otherwise live cache entry. */
    f=fopen("/etc/resolv.conf","w");check(f!=NULL);
    if(f){fputs("nameserver 192.168.64.1\nnameserver 192.168.64.1\n",f);fclose(f);}
    lookup("cache.test","cache.test");
    printf("DNS_CACHE checks=%u failures=%u\n",checks,failures);
    return failures?1:0;
}
