/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Device contract checks, run only inside the disposable test guest. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/pcm.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { printf("MUSIC_CHECK_FAILED line=%d errno=%d\n", __LINE__, errno); return 1; } } while (0)
int main(void) {
    int fd, input, child, status;
    unsigned char *allocation, *buffer;
    unsigned long boundary;
    struct pcm_format format;
    struct pcm_music_data data;
    struct pcm_music_control control;
    struct pcm_music_state state;
    struct pcm_mix mix;
    struct pcm_mix_voice voice;
    unsigned char octets[4] = {0,64,128,255};
    short mixed[16];
    static short expected[8] = {-32768,-32768,-16384,-16384,0,0,32512,32512};
    unsigned i;
    fd = open("/dev/pcm", O_WRONLY | O_NONBLOCK); CHECK(fd >= 0);
    control.version = PCM_VERSION; control.action = PCM_MUSIC_VOLUME; control.value = 100;
    CHECK(ioctl(fd, PCM_MUSIC_CONTROL, &control) < 0 && errno == EINVAL);
    format.version = PCM_VERSION; format.channels = 2; format.rate = 22050UL; format.capacity = 4096UL;
    CHECK(!ioctl(fd, PCM_CONFIGURE, &format));
    voice.data=octets;voice.length=4;voice.offset=voice.fraction=0;
    voice.step=32768UL;voice.left=127;voice.right=0;
    mix.version=PCM_VERSION;mix.frames=8;mix.count=1;mix.voices=&voice;mix.samples=mixed;
    CHECK(!ioctl(fd,PCM_MIX,&mix));
    for(i=0;i<8;i++){CHECK(mixed[i*2]==expected[i]);CHECK(!mixed[i*2+1]);}
    CHECK(voice.offset==4&&!voice.fraction);
    voice.offset=0;mix.samples=(short *)&voice;
    CHECK(ioctl(fd,PCM_MIX,&mix)<0&&errno==EINVAL);
    mix.samples=mixed;
    voice.offset=0;voice.left=128;
    CHECK(ioctl(fd,PCM_MIX,&mix)<0&&errno==EINVAL);
    voice.left=127;voice.data=(void *)0xffffffffUL;
    CHECK(ioctl(fd,PCM_MIX,&mix)<0&&errno==EFAULT);
    voice.data=octets;
    allocation = malloc(130000UL); CHECK(allocation != NULL);
    boundary = 64000UL - (unsigned)(unsigned long)allocation;
    if (boundary < 16UL) boundary += 64000UL;
    buffer = allocation + boundary - 16UL;
    input = open("/mnt/host/bank.bin", O_RDONLY); CHECK(input >= 0);
    CHECK(read(input, buffer, 6308U) == 6308); close(input);
    data.version = 2; data.data = buffer; data.length = 6308UL;
    CHECK(ioctl(fd, PCM_MUSIC_BANK, &data) < 0 && errno == EINVAL);
    data.version = PCM_VERSION; data.length = 1;
    CHECK(ioctl(fd, PCM_MUSIC_BANK, &data) < 0 && errno == EINVAL);
    data.length = 65536UL;
    CHECK(ioctl(fd, PCM_MUSIC_SCORE, &data) < 0 && errno == EINVAL);
    data.length = 6308UL; data.data = (void *)0xffffffffUL;
    CHECK(ioctl(fd, PCM_MUSIC_BANK, &data) < 0 && errno == EFAULT);
    data.data = buffer;
    CHECK(!ioctl(fd, PCM_MUSIC_BANK, &data));
    memset(buffer, 0, 6308UL);
    CHECK(!ioctl(fd, PCM_MUSIC_STATE, &state) && (state.flags & 1));
    input = open("/mnt/host/score.bin", O_RDONLY); CHECK(input >= 0);
    status = read(input, buffer, 65535U); CHECK(status > 16); close(input);
    data.length = (unsigned)status;
    CHECK(!ioctl(fd, PCM_MUSIC_SCORE, &data));
    memset(buffer, 0, data.length);
    CHECK(ioctl(fd, PCM_MUSIC_SCORE, &data) < 0 && errno == EINVAL);
    CHECK(!ioctl(fd, PCM_MUSIC_STATE, &state) && (state.flags & 3) == 3);
    control.value = 128;
    CHECK(ioctl(fd, PCM_MUSIC_CONTROL, &control) < 0 && errno == EINVAL);
    control.value = 100;
    CHECK(!ioctl(fd, PCM_MUSIC_CONTROL, &control));
    child = fork(); CHECK(child >= 0);
    if (!child) {
        if (ioctl(fd, PCM_MUSIC_CONTROL, &control) >= 0 || errno != EPERM) _exit(1);
        if (ioctl(fd, PCM_MUSIC_BANK, &data) >= 0 || errno != EPERM) _exit(2);
        if (ioctl(fd, PCM_MIX, &mix) >= 0 || errno != EPERM) _exit(3);
        close(fd); _exit(0);
    }
    CHECK(waitpid(child, &status, 0) == child && !status);
    control.action = PCM_MUSIC_PLAY; control.value = 1;
    CHECK(!ioctl(fd, PCM_MUSIC_CONTROL, &control));
    CHECK(!ioctl(fd, PCM_MUSIC_STATE, &state) && (state.flags & PCM_MUSIC_PLAYING));
    control.action = PCM_MUSIC_PAUSE; control.value = 0;
    CHECK(!ioctl(fd, PCM_MUSIC_CONTROL, &control));
    CHECK(!ioctl(fd, PCM_MUSIC_STATE, &state) && (state.flags & 8));
    control.action = PCM_MUSIC_RESUME;
    CHECK(!ioctl(fd, PCM_MUSIC_CONTROL, &control));
    control.action = PCM_MUSIC_STOP;
    CHECK(!ioctl(fd, PCM_MUSIC_CONTROL, &control));
    close(fd); free(allocation);
    fd = open("/dev/pcm", O_WRONLY); CHECK(fd >= 0); close(fd);
    puts("MUSIC_DEVICE_OK"); return 0;
}
