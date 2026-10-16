/* Test the production DMX decoder/mixer against explicit sample values. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/pcm_sound.c"
static byte lump_data[44];
static unsigned int reads, releases;
uint32_t Doom_Tick(void) { return 0; }
uint32_t W_LumpLength(unsigned int lump) { (void)lump;return sizeof(lump_data); }
void *W_CacheLumpNum(int lump,int tag) { (void)lump;(void)tag;++reads;return lump_data; }
void W_ReleaseLumpNum(int lump) { (void)lump;++releases; }
int W_GetNumForName(char *name) { if(strcmp(name,"dspistol"))exit(2);return 1; }
void I_Error(char *error,...) {puts(error);exit(1);}
static void check(int condition) {if(!condition){puts("PCM MIXER FAIL");exit(1);}}
int main(void) {
    sfxinfo_t sfx;
    unsigned int i;
    static short expected[8]={-32768,-32768,-16384,-16384,0,0,32512,32512};
    memset(&sfx,0,sizeof(sfx));strcpy(sfx.name,"pistol");sfx.lumpnum=1;
    memset(lump_data,128,sizeof(lump_data));
    lump_data[0]=3;lump_data[1]=0;lump_data[2]=17;lump_data[3]=43; /* 11025 */
    lump_data[4]=36;lump_data[5]=lump_data[6]=lump_data[7]=0;
    lump_data[24]=0;lump_data[25]=64;lump_data[26]=128;lump_data[27]=255;
    check(doom_pcm_module.Init(true));check(doom_pcm_module.GetSfxLumpNum(&sfx)==1);
    check(doom_pcm_module.StartSound(&sfx,0,127,0)==0);
    check(doom_pcm_module.SoundIsPlaying(0));mix_block();
    for(i=0;i<8;++i){check(mixed[i*2]==expected[i]);check(mixed[i*2+1]==0);}
    check(mixed[16]==0 && !doom_pcm_module.SoundIsPlaying(0));
    check(doom_pcm_module.StartSound(&sfx,0,127,254)==0);mix_block();
    for(i=0;i<8;++i){check(mixed[i*2]==0);check(mixed[i*2+1]==expected[i]);}
    for(i=0;i<3;++i)check(doom_pcm_module.StartSound(&sfx,(int)i,127,0)==(int)i);
    mix_block();check(mixed[0]==-32768 && mixed[12]==32767);check(reads==1);
    check(doom_pcm_module.StartSound(&sfx,0,127,0)==0);doom_pcm_module.StopSound(0);mix_block();check(mixed[0]==0);
    doom_pcm_module.Shutdown();check(releases==1);
    lump_data[0]=0;check(doom_pcm_module.Init(true));check(doom_pcm_module.StartSound(&sfx,0,127,0)==-1);check(releases==2);
    doom_pcm_module.Shutdown();
    puts("PCM MIXER OK signed samples, guard trimming, rate conversion, panning, clipping, cache and malformed records");return 0;
}
