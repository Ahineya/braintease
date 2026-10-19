#ifndef FUSE_MUSIC_H
#define FUSE_MUSIC_H
void *music_new(void);
void music_free(void *);
void music_stop(void *);
int music_load(void *,const char *);
void music_step(void *);
void music_pause(void *);
int music_seek(void *,long);
void music_volume(void *,unsigned);
unsigned music_state(void *);
unsigned music_gain(void *);
unsigned music_channels(void *);
unsigned long music_rate(void *);
unsigned long music_elapsed(void *);
unsigned long music_duration(void *);
unsigned long music_underruns(void *);
const char *music_status(void *);
const char *music_title(void *);
#endif
