#include <stdarg.h>

struct Pair {int tag; long value;};

long sum(int count,...) {
    va_list ap;long result=0;
    va_start(ap,count);
    for(int i=0;i<count;i++)result+=va_arg(ap,long);
    va_end(ap);
    return result;
}
int check(int marker,long fixed,...) {
    va_list ap,copy;
    va_start(ap,fixed);
    if(marker!=7 || fixed!=70000L)return 1;
    if(va_arg(ap,int)!=-13)return 2;
    va_copy(copy,ap);
    if(va_arg(copy,unsigned long long)!=0x123456789abcdef0ULL)return 3;
    if(va_arg(ap,unsigned long long)!=0x123456789abcdef0ULL)return 4;
    if(va_arg(ap,double)!=2.25)return 5;
    if(va_arg(ap,double)!=1.5)return 6;
    if(*va_arg(ap,int*)!=19)return 7;
    if(va_arg(ap,long (*)(int,...))!=sum)return 8;
    va_end(copy);va_end(ap);
    return 0;
}
struct Pair pairs(int count,...) {
    struct Pair result={0,0L};va_list ap;
    va_start(ap,count);
    for(int i=0;i<count;i++) {
        struct Pair p=va_arg(ap,struct Pair);
        result.tag+=p.tag;result.value+=p.value;
    }
    va_end(ap);return result;
}
int direct_builtins(int unused,...) {
    va_list ap,copy;
    __builtin_va_start(ap,unused);
    __builtin_va_copy(copy,ap);
    if(__builtin_va_arg(copy,int)!=17 || __builtin_va_arg(ap,int)!=17)return 1;
    __builtin_va_end(copy);__builtin_va_end(ap);return 0;
}
int main(void) {
    int local=19,status;
    long (*callback)(int,...)=sum;
    struct Pair a={2,70000L},b={3,5L},p;
    if(sum(0)!=0L || callback(4,0L,1L,70000L,65536L)!=135537L)return 1;
    status=check(7,70000L,-13,0x123456789abcdef0ULL,2.25,1.5f,&local,sum);
    if(status)return 20+status;
    if(direct_builtins(0,17)!=0)return 3;
    p=pairs(2,a,b);
    if(p.tag!=5 || p.value!=70005L || a.tag!=2)return 4;
    return 0;
}
