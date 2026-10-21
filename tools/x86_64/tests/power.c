#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../boot/power.h"
static uint8_t f[276];
static void checksum(void){unsigned s=0;f[9]=0;for(unsigned i=0;i<sizeof(f);i++)s+=f[i];f[9]=-s;}
static void setup(void){memset(f,0,sizeof(f));memcpy(f,"FACP",4);f[4]=20;f[5]=1;f[113]=4;f[116]=1;f[117]=8;f[119]=1;f[120]=0xb2;f[128]=0xfb;checksum();}
int main(int argc,char **argv){
    setup();PowerReset r=power_reset_parse(f,sizeof(f));assert(r.valid&&r.port==0xb2&&r.value==0xfb);
    for(unsigned i=0;i<129;i++)assert(!power_reset_parse(f,i).valid);
    unsigned fields[]={0,4,113,116,117,118,119,122,127};
    for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);i++){setup();f[fields[i]]=fields[i]==113?0:fields[i]==119?2:0xff;checksum();assert(!power_reset_parse(f,sizeof(f)).valid);}
    setup();f[9]++;assert(!power_reset_parse(f,sizeof(f)).valid);
    setup();f[120]=0;checksum();assert(!power_reset_parse(f,sizeof(f)).valid);
    setup();f[120]=0xf9;f[121]=0x0c;f[128]=6;f[119]=0;checksum();r=power_reset_parse(f,sizeof(f));assert(r.valid&&r.port==0xcf9&&r.value==6);
    if(argc==2){uint8_t actual[1024];FILE *in=fopen(argv[1],"rb");assert(in);size_t n=fread(actual,1,sizeof(actual),in);assert(feof(in)&&!ferror(in));fclose(in);r=power_reset_parse(actual,n);assert(n==276&&r.valid&&r.port==0xb2&&r.value==0xfb);}
    puts("ACPI reset guards passed: HP/QEMU descriptors, bounds, checksum, flags, address/width/offset/access rejection");
}
