#include <stdlib.h>
#include <string.h>
#include <stdint.h>
/* Both failed searches return the SDK's tagged null, rather than (0, 0). */
int main(void) {
    const char *text="abc";
    const char *p=strrchr(text,'/');
    if(p || p!=NULL || (uintptr_t)p!=0) return 1;
    if(strstr(text,"xyz")) return 2;
    if(strstr(text,"bc")!=text+1) return 3;
    p=(const char *)(uintptr_t)0;
    if(p || p!=strchr(text,'z')) return 4;
    free((void *)p);
    return 0;
}
