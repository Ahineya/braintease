#include <vm_music.h>
#include <mmio.h>
static unsigned short music_command(unsigned short command, volatile unsigned short *desc) {
    unsigned long p=(unsigned long)desc;
    mmio_write(MMIO_DEVICE_DESC_BANK,(unsigned short)(p>>16));
    mmio_write(MMIO_DEVICE_DESC_OFF,(unsigned short)p);
    mmio_write(MMIO_DEVICE_COMMAND,command);
    return mmio_read(MMIO_DEVICE_STATUS);
}
static unsigned short music_data(unsigned short command,const unsigned char *data,unsigned long length) {
    unsigned long p=(unsigned long)data;
    volatile unsigned short d[8];
    d[0]=1;d[1]=8;d[2]=(unsigned short)(p>>16);d[3]=(unsigned short)p;
    d[4]=(unsigned short)length;d[5]=(unsigned short)(length>>16);d[6]=d[7]=0;
    return music_command(command,d);
}
unsigned short vm_music_bank(const unsigned char *data,unsigned long length) {return music_data(VM_MUSIC_BANK,data,length);}
unsigned short vm_music_score(const unsigned char *data,unsigned long length) {return music_data(VM_MUSIC_SCORE,data,length);}
unsigned short vm_music_control(unsigned short action,unsigned short value) {
    volatile unsigned short d[4];d[0]=1;d[1]=4;d[2]=action;d[3]=value;
    return music_command(VM_MUSIC_CONTROL,d);
}
unsigned short vm_music_state(VmMusicState *state) {
    volatile unsigned short d[10];unsigned short i,status;
    d[0]=1;d[1]=10;for(i=2;i<10;++i)d[i]=0;
    status=music_command(VM_MUSIC_STATE,d);
    if(status==VM_DEVICE_OK) {
        state->flags=d[2];state->volume=d[3];
        state->position=(unsigned long long)d[4] | ((unsigned long long)d[5]<<16)
            | ((unsigned long long)d[6]<<32) | ((unsigned long long)d[7]<<48);
    }
    return status;
}
