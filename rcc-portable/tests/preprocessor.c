#define PP_HEADER "preprocessor.h"
#include PP_HEADER
#include "preprocessor.h"

static long sum(int count,...) {
    va_list args;long result=0;int i;
    va_start(args,count);
    for(i=0;i<count;i++) result+=va_arg(args,long);
    va_end(args);return result;
}
int main(void) {
    struct portable_pair value={PP_CHOICE,PP_MAGIC};
    const char *text=PP_TEXT(PP_MAGIC);
    if(value.tag!=7 || value.value!=70000UL) return 1;
    if(PP_PLUS(PP_PLUS(3))!=5) return 2;
    if(PP_APPLY(sum,3,1L,2L,70000L)!=70003L) return 3;
    if(offsetof(struct portable_pair,value)!=1 || sizeof(value)!=3) return 4;
    if(text[0]!='(' || text[1]!='7') return 5;
    return 0;
}
