#include <limits.h>
int puts(const char *);
static const char *message(void) {return "portable C11 frontend";}
char wide[]={'\x1234',0};
char escaped[]="\x1234";
int main(void) {
    char local[]="hello";
    _Bool b=256;
    if(CHAR_BIT!=16 || !b || local[4]!='o' || wide[0]!=0x1234 || escaped[0]!=0x1234)return 1;
    puts(message());
    return 0;
}
