#ifndef FUSE_MUSIC_WAV_H
#define FUSE_MUSIC_WAV_H
typedef struct Wav {
    int fd;
    unsigned channels,bits,align;
    unsigned long rate,offset,bytes,frames,position;
} Wav;
int wav_open(Wav *,const char *,char *,unsigned);
void wav_close(Wav *);
long wav_read(Wav *,short *,unsigned);
int wav_seek(Wav *,unsigned long);
#endif
