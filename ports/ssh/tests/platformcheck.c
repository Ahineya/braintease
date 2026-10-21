/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>
extern int riplix_ssh_size_string(char *,unsigned,unsigned long,unsigned long);
static int failed;
static void check(unsigned long low,unsigned long high,const char *expected) {
    char out[32];int n=riplix_ssh_size_string(out,sizeof(out),low,high);
    if(n==(int)strlen(expected)&&!strcmp(out,expected))putchar('Y');
    else {putchar('N');failed=1;}
}
int main(void) {
    char guard[2]={'!','!'};
    check(0,0,"       0");check(1,0,"       1");check(98414,0,"   98414");
    check(2147483647UL,0,"2147483647");check(4294967295UL,0,"4294967295");
    check(0,1,"4294967296");check(4294967295UL,4294967295UL,"18446744073709551615");
    if(riplix_ssh_size_string(guard,1,1,0)<0&&guard[0]=='!'&&guard[1]=='!')putchar('Y');
    else {putchar('N');failed=1;}
    if(riplix_ssh_size_string(0,32,1,0)<0)putchar('Y');else {putchar('N');failed=1;}
    putchar('\n');return failed;
}
