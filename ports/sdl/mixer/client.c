/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Optional Unix process frontend. The helper owns immutable uploaded samples,
 * voices and PCM; no guest pointers, callbacks or C structures cross exec. */
#include "SDL_mixer.h"
#include "audio_formats.h"
#include "audio.h"
#include "platform.h"
#include "ipc.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

#include "chunk.inc"

typedef struct ClientChunk {
    Mix_Chunk *chunk;
    Uint32 id, session;
    struct ClientChunk *next;
} ClientChunk;
static ClientChunk *chunks;
static SDL_AudioSpec device;
static int command_fd=-1, reply_fd=-1, opened, busy, dispatching, closing;
static pid_t child, owner;
static Uint32 serial, session, next_chunk;
static void (SDLCALL *finished_hook)(int);
static Uint8 *finished;
static int finished_capacity;
static Uint8 poll_tx[MX_HEADER], poll_rx[MX_HEADER+MX_PAYLOAD];
static Uint32 poll_sent, poll_read, poll_needed, poll_request, poll_started;
static int poll_active;

static void aux_pump(void);
static void close_all(void);
static Uint32 aux_wait(Uint32 maximum) {
    return opened && finished_hook && maximum>20 ? 20 : maximum;
}
static int belongs(void) { return owner==getpid(); }
static Sint32 write_pipe(int fd,const Uint8 *p,Uint32 n) {
    void (*previous)(int)=signal(SIGPIPE,SIG_IGN);
    Sint32 result=(Sint32)write(fd,p,n);
    int saved=errno;
    if(previous!=SIG_ERR) signal(SIGPIPE,previous);
    errno=saved;
    return result;
}
static void pause_ms(Uint32 ms) {
    struct timeval delay;
    delay.tv_sec=ms/1000;delay.tv_usec=(ms%1000)*1000;
    select(0,NULL,NULL,NULL,&delay);
}
static void reap(void) {
    Uint32 start;
    int status;
    pid_t result;
    if(child<=0||!belongs()) {child=0;return;}
    start=SDL_GX_GetTicks();
    do {
        result=waitpid(child,&status,WNOHANG);
        if(result==child||(result<0&&errno==ECHILD)){child=0;return;}
        if(result<0&&errno!=EINTR)break;
        pause_ms(5);
    }while(SDL_GX_GetTicks()-start<250);
    kill(child,SIGKILL);
    start=SDL_GX_GetTicks();
    do {
        result=waitpid(child,&status,WNOHANG);
        if(result==child||(result<0&&errno==ECHILD)){child=0;return;}
        pause_ms(5);
    }while(SDL_GX_GetTicks()-start<1000);
    child=0;
}
static void detach(void) {
    if(command_fd>=0)close(command_fd);
    if(reply_fd>=0)close(reply_fd);
    command_fd=reply_fd=-1;opened=0;poll_active=0;
    SDL_C16AudioAuxPump=NULL;SDL_C16AudioAuxQuit=NULL;SDL_C16AudioAuxWait=NULL;
    reap();
}
static int failed(const char *message) {
    detach();SDL_SetError("PCM mixer helper: %s",message);return -1;
}
static int ready(int fd,int writing,Uint32 started) {
    fd_set set;
    struct timeval timeout;
    int result;
    if(SDL_GX_GetTicks()-started>=MX_TIMEOUT)return failed("IPC timed out");
    FD_ZERO(&set);FD_SET(fd,&set);
    timeout.tv_sec=0;timeout.tv_usec=20000;
    result=select(fd+1,writing?NULL:&set,writing?&set:NULL,NULL,&timeout);
    if(result<0&&errno!=EINTR)return failed("IPC readiness failed");
    return result;
}
static int transfer(int fd,Uint8 *buffer,Uint32 count,int writing,Uint32 start) {
    Uint32 done=0;
    Sint32 n;
    while(done<count) {
        n=writing?write_pipe(fd,buffer+done,count-done):(Sint32)read(fd,buffer+done,count-done);
        if(n>0){done+=(Uint32)n;continue;}
        if(!n)return failed("helper closed its IPC stream");
        if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)return failed("IPC read/write failed");
        if(ready(fd,writing,start)<0)return -1;
    }
    return 0;
}
static int ensure_finished(int capacity) {
    Uint8 *next;
    if(capacity<=finished_capacity)return 0;
    next=SDL_calloc((Uint32)capacity,sizeof(*next));
    if(!next){SDL_OutOfMemory();return -1;}
    if(finished_capacity)SDL_memcpy(next,finished,(Uint32)finished_capacity*sizeof(*next));
    SDL_free(finished);finished=next;finished_capacity=capacity;
    return 0;
}
static int queue_finished(const MX_Message *response) {
    Uint32 at,index;
    if(response->length%4)return failed("malformed completion batch");
    for(at=0;at<response->length;at+=4) {
        index=mx_get32(response->payload+at);
        if(index>=(Uint32)finished_capacity)return failed("invalid completion channel");
        finished[index]=1;
    }
    return 0;
}
static void dispatch_finished(void) {
    int i,budget=finished_capacity;
    if(dispatching||busy)return;
    dispatching=1;
    for(i=0;i<finished_capacity&&budget;i++)if(finished[i]) {
        finished[i]=0;budget--;
        if(finished_hook)finished_hook(i);
    }
    dispatching=0;
    if(!opened){SDL_free(finished);finished=NULL;finished_capacity=0;}
}
/* Progress a single asynchronous completion poll; never wait at an SDL draw or
 * input boundary. A subsequent synchronous command first finishes this poll. */
static int progress_poll(int wait) {
    Sint32 n;
    MX_Message response;
    /* Time spent computing in the application is not an IPC timeout. A reply
     * may have been waiting throughout that work; only bound an actual wait. */
    if(wait)poll_started=SDL_GX_GetTicks();
    while(poll_active) {
        if(wait&&SDL_GX_GetTicks()-poll_started>=MX_TIMEOUT)return failed("completion poll timed out");
        if(poll_sent<MX_HEADER) {
            n=write_pipe(command_fd,poll_tx+poll_sent,MX_HEADER-poll_sent);
            if(n>0){poll_sent+=(Uint32)n;continue;}
            if(!n||(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR))return failed("completion pipe closed");
            if(!wait)return 0;
            if(ready(command_fd,1,poll_started)<0)return -1;
            continue;
        }
        n=(Sint32)read(reply_fd,poll_rx+poll_read,poll_needed-poll_read);
        if(n>0) {
            poll_read+=(Uint32)n;
            if(poll_read==MX_HEADER&&poll_needed==MX_HEADER) {
                if(mx_decode(&response,poll_rx)<0||response.request!=poll_request||response.operation!=(MX_POLL|MX_REPLY)||response.arg[0]<0)
                    return failed("invalid completion reply");
                poll_needed=MX_HEADER+response.length;
            }
            if(poll_read==poll_needed) {
                if(mx_decode(&response,poll_rx)<0)return failed("invalid completion header");
                SDL_memcpy(response.payload,poll_rx+MX_HEADER,response.length*sizeof(Uint8));
                poll_active=0;
                return queue_finished(&response);
            }
            continue;
        }
        if(!n||(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR))return failed("completion reply closed");
        if(!wait)return 0;
        if(ready(reply_fd,0,poll_started)<0)return -1;
    }
    return 0;
}
static Sint32 rpc(MX_Message *message) {
    Uint8 header[MX_HEADER];
    Uint32 start=SDL_GX_GetTicks(),request,operation;
    if(!belongs()){SDL_SetError("PCM mixer belongs to another process");return -1;}
    if(command_fd<0){SDL_SetError("PCM mixer helper is not connected");return -1;}
    if(busy){SDL_SetError("PCM mixer IPC is already in progress");return -1;}
    busy=1;
    if(progress_poll(1)<0)goto fail;
    if(++serial==0)++serial;
    request=serial;operation=message->operation;
    message->request=request;
    mx_encode(header,message);
    if(transfer(command_fd,header,MX_HEADER,1,start)<0||
       (message->length&&transfer(command_fd,message->payload,message->length,1,start)<0)||
       transfer(reply_fd,header,MX_HEADER,0,start)<0)goto fail;
    if(mx_decode(message,header)<0||message->request!=request||message->operation!=(operation|MX_REPLY)) {
        failed("invalid response framing");goto fail;
    }
    if(message->length&&transfer(reply_fd,message->payload,message->length,0,start)<0)goto fail;
    if(message->arg[0]<0) {
        char error[MX_PAYLOAD+1];Uint32 i;
        for(i=0;i<message->length;i++)error[i]=(char)message->payload[i];
        error[message->length]=0;
        SDL_SetError("PCM mixer helper: %s",message->length?error:"command failed");
    }
    busy=0;return message->arg[0];
fail:
    busy=0;return -1;
}
static int command(Uint32 operation,Sint32 a,Sint32 b) {
    MX_Message message;int result;
    SDL_memset(&message,0,sizeof(message));message.operation=operation;message.arg[0]=a;message.arg[1]=b;
    result=rpc(&message);dispatch_finished();return result;
}
static void aux_pump(void) {
    MX_Message message;
    if(busy||dispatching||closing||!opened||!belongs())return;
    if(!poll_active&&finished_hook) {
        SDL_memset(&message,0,sizeof(message));
        if(++serial==0)++serial;
        message.request=poll_request=serial;message.operation=MX_POLL;
        mx_encode(poll_tx,&message);poll_sent=poll_read=0;poll_needed=MX_HEADER;
        poll_started=SDL_GX_GetTicks();poll_active=1;
    }
    if(poll_active)progress_poll(0);
    dispatch_finished();
}
static int descriptors(int fd) {
    return fd>=0&&fd<FD_SETSIZE&&fcntl(fd,F_SETFD,FD_CLOEXEC)>=0&&
        fcntl(fd,F_SETFL,fcntl(fd,F_GETFL,0)|O_NONBLOCK)>=0?0:-1;
}
static int spawn_helper(void) {
    int requests[2],replies[2],saved,read_fd,write_fd,fd;
    const char *path=getenv("SDL_MIXER_HELPER");
    char *args[]={"sdl-mixerd","--ipc","0","1",NULL};
    if(pipe(requests)<0){SDL_SetError("Cannot create mixer command pipe");return -1;}
    if(pipe(replies)<0){close(requests[0]);close(requests[1]);SDL_SetError("Cannot create mixer reply pipe");return -1;}
    if(descriptors(requests[1])<0||descriptors(replies[0])<0)goto fail;
    owner=getpid();child=fork();
    if(child<0)goto fail;
    if(!child) {
        read_fd=fcntl(requests[0],F_DUPFD,3);write_fd=fcntl(replies[1],F_DUPFD,3);
        if(read_fd<0||write_fd<0||dup2(read_fd,0)<0||dup2(write_fd,1)<0)_exit(126);
        fcntl(0,F_SETFD,0);fcntl(1,F_SETFD,0);
        if(requests[0]>1)close(requests[0]);if(requests[1]>1)close(requests[1]);
        if(replies[0]>1)close(replies[0]);if(replies[1]>1)close(replies[1]);
        for(fd=3;fd<FD_SETSIZE;fd++)close(fd);
        if(path&&*path)execv(path,args);
        else {execv(MX_DEFAULT_HELPER,args);execv("/bin/sdl-mixerd",args);}
        _exit(127);
    }
    close(requests[0]);close(replies[1]);
    command_fd=requests[1];reply_fd=replies[0];return 0;
fail:
    saved=errno;close(requests[0]);close(requests[1]);close(replies[0]);close(replies[1]);
    errno=saved;SDL_SetError("Cannot start PCM mixer helper");return -1;
}
Sint32 SDLCALL Mix_PCMHelperPID(void) {return belongs()?(Sint32)child:0;}
int SDLCALL Mix_OpenAudio(Sint32 rate,Uint16 format,int channels,int samples) {
    MX_Message message;static int registered;
    if(opened&&belongs()) {
        if(rate!=device.freq||format!=device.format||channels!=device.channels||opened==32767) {
            SDL_SetError("PCM mixer is already open with a different specification");return -1;
        }
        opened++;return 0;
    }
    if(command_fd>=0)detach();
    if(SDL_InitSubSystem(SDL_INIT_AUDIO)<0)return -1;
    if(ensure_finished(MIX_CHANNELS)<0)return -1;
    if(spawn_helper()<0){dispatch_finished();return -1;}
    SDL_memset(&message,0,sizeof(message));message.operation=MX_OPEN;
    message.arg[0]=rate;message.arg[1]=format;message.arg[2]=channels;message.arg[3]=samples;
    if(rpc(&message)<0){detach();dispatch_finished();return -1;}
    SDL_memset(&device,0,sizeof(device));device.freq=message.arg[1];device.format=(Uint16)message.arg[2];device.channels=(Uint8)message.arg[3];
    if(++session==0)++session;
    SDL_memset(finished,0,(Uint32)finished_capacity*sizeof(*finished));
    opened=1;closing=0;next_chunk=0;
    SDL_C16AudioAuxPump=aux_pump;SDL_C16AudioAuxQuit=close_all;SDL_C16AudioAuxWait=aux_wait;
    if(!registered){atexit(close_all);registered=1;}
    return 0;
}
void SDLCALL Mix_CloseAudio(void) {
    MX_Message message;int rounds;
    if(closing)return;
    if(!belongs()){detach();return;}
    if(opened>1){opened--;return;}
    closing=1;
    if(opened&&command_fd>=0) {
        SDL_memset(&message,0,sizeof(message));message.operation=MX_HALT;message.arg[0]=-1;rpc(&message);
        /* Pending notifications are coalesced per channel by the helper. */
        for(rounds=0;command_fd>=0&&rounds<128;rounds++) {
            SDL_memset(&message,0,sizeof(message));message.operation=MX_POLL;
            if(rpc(&message)<0||queue_finished(&message)<0||!message.length)break;
        }
        if(command_fd>=0){SDL_memset(&message,0,sizeof(message));message.operation=MX_QUIT;rpc(&message);}
    }
    detach();closing=0;dispatch_finished();
}
static void close_all(void) {if(opened)opened=1;Mix_CloseAudio();}
int SDLCALL Mix_QuerySpec(Sint32 *rate,Uint16 *format,int *channels) {
    if(!opened||!belongs())return 0;
    if(rate)*rate=device.freq;if(format)*format=device.format;if(channels)*channels=device.channels;
    return opened;
}
int SDLCALL Mix_AllocateChannels(int count) {
    if(count>finished_capacity&&ensure_finished(count)<0)return -1;
    return command(MX_ALLOCATE,count,0);
}
int SDLCALL Mix_ReserveChannels(int count){return command(MX_RESERVE,count,0);}
static ClientChunk *find_chunk(Mix_Chunk *chunk) {
    ClientChunk *entry;
    for(entry=chunks;entry;entry=entry->next)if(entry->chunk==chunk)return entry;
    return NULL;
}
static Mix_Chunk *upload(Mix_Chunk *chunk) {
    ClientChunk *entry;
    MX_Message message;Uint32 offset,amount,id;
    if(!chunk)return NULL;
    entry=SDL_malloc(sizeof(*entry));
    if(!entry){SDL_OutOfMemory();goto fail;}
    if(next_chunk==0x7fffffffUL){SDL_SetError("PCM chunk identities exhausted");SDL_free(entry);goto fail;}
    id=++next_chunk;
    SDL_memset(&message,0,sizeof(message));message.operation=MX_BEGIN;
    message.arg[0]=(Sint32)id;message.arg[1]=(Sint32)chunk->alen;message.arg[2]=chunk->volume;
    if(rpc(&message)<0){SDL_free(entry);goto fail;}
    for(offset=0;offset<chunk->alen;offset+=amount) {
        amount=chunk->alen-offset;if(amount>MX_PAYLOAD)amount=MX_PAYLOAD;
        SDL_memset(&message,0,sizeof(message));message.operation=MX_DATA;message.arg[0]=(Sint32)id;message.arg[1]=(Sint32)offset;message.length=amount;
        SDL_memcpy(message.payload,chunk->abuf+offset,amount*sizeof(Uint8));
        if(rpc(&message)<0)goto cancel;
    }
    SDL_memset(&message,0,sizeof(message));message.operation=MX_COMMIT;message.arg[0]=(Sint32)id;
    if(rpc(&message)<0)goto cancel;
    entry->chunk=chunk;entry->id=id;entry->session=session;entry->next=chunks;chunks=entry;
    dispatch_finished();return chunk;
cancel:
    SDL_free(entry);
    if(command_fd>=0){SDL_memset(&message,0,sizeof(message));message.operation=MX_DROP;message.arg[0]=(Sint32)id;rpc(&message);}
fail:
    if(chunk->allocated)SDL_free(chunk->abuf);SDL_free(chunk);dispatch_finished();return NULL;
}
Mix_Chunk * SDLCALL Mix_LoadWAV_RW(SDL_RWops *src,int freesrc) {
    if(!opened||!belongs()){SDL_SetError("PCM mixer is not open");if(src&&freesrc)SDL_RWclose(src);return NULL;}
    return upload(mix_load_wav(src,freesrc,device));
}
Mix_Chunk * SDLCALL Mix_QuickLoad_RAW(Uint8 *mem,Uint32 length) {
    Mix_Chunk *chunk;Uint32 frame_bytes=(device.format&255U)/8*device.channels;
    if(!opened||!belongs()||!mem||!length||!frame_bytes||length%frame_bytes||length>0x7fffffffUL/sizeof(Uint8)) {
        SDL_SetError("RAW PCM requires open audio and frame-aligned storage");return NULL;
    }
    chunk=SDL_malloc(sizeof(*chunk));if(!chunk){SDL_OutOfMemory();return NULL;}
    chunk->allocated=0;chunk->abuf=mem;chunk->alen=length;chunk->volume=128;
    return upload(chunk);
}
void SDLCALL Mix_FreeChunk(Mix_Chunk *chunk) {
    ClientChunk **link=&chunks,*entry;
    while((entry=*link)!=NULL&&entry->chunk!=chunk)link=&entry->next;
    if(!entry)return;
    *link=entry->next;
    if(opened&&belongs()&&entry->session==session) {
        MX_Message message;SDL_memset(&message,0,sizeof(message));message.operation=MX_DROP;message.arg[0]=(Sint32)entry->id;rpc(&message);
    }
    if(chunk->allocated)SDL_free(chunk->abuf);
    SDL_free(chunk);SDL_free(entry);dispatch_finished();
}
static int play(int channel,Mix_Chunk *chunk,int loops,Sint32 ticks,Sint32 fade) {
    ClientChunk *entry=find_chunk(chunk);MX_Message message;int result;
    if(!opened||!entry||entry->session!=session){SDL_SetError("PCM chunk belongs to another helper session");return -1;}
    SDL_memset(&message,0,sizeof(message));message.operation=MX_CHUNK_VOLUME;message.arg[0]=(Sint32)entry->id;message.arg[1]=chunk->volume>128?128:chunk->volume;
    if(rpc(&message)<0)return -1;
    SDL_memset(&message,0,sizeof(message));message.operation=MX_PLAY;message.arg[0]=channel;message.arg[1]=(Sint32)entry->id;message.arg[2]=loops;message.arg[3]=ticks;message.arg[4]=fade;
    result=rpc(&message);dispatch_finished();return result;
}
int SDLCALL Mix_PlayChannelTimed(int c,Mix_Chunk *s,int loops,Sint32 ticks){return play(c,s,loops,ticks,-1);}
int SDLCALL Mix_FadeInChannelTimed(int c,Mix_Chunk *s,int loops,Sint32 ms,Sint32 ticks){return play(c,s,loops,ticks,ms);}
int SDLCALL Mix_HaltChannel(int c){return command(MX_HALT,c,0);}
int SDLCALL Mix_ExpireChannel(int c,Sint32 ms){return command(MX_EXPIRE,c,ms);}
int SDLCALL Mix_Volume(int c,int v){return command(MX_VOLUME,c,v);}
int SDLCALL Mix_VolumeChunk(Mix_Chunk *chunk,int volume) {
    ClientChunk *entry=find_chunk(chunk);int previous;
    if(!entry)return -1;
    previous=chunk->volume;
    if(volume>=0)chunk->volume=(Uint8)(volume>128?128:volume);
    if(opened&&entry->session==session)command(MX_CHUNK_VOLUME,(Sint32)entry->id,volume);
    return previous;
}
void SDLCALL Mix_Pause(int c){command(MX_PAUSE,c,0);}
void SDLCALL Mix_Resume(int c){command(MX_RESUME,c,0);}
int SDLCALL Mix_Paused(int c){return command(MX_PAUSED,c,0);}
int SDLCALL Mix_Playing(int c){return command(MX_PLAYING,c,0);}
int SDLCALL Mix_FadeOutChannel(int c,Sint32 ms){return command(MX_FADEOUT,c,ms);}
Mix_Fading SDLCALL Mix_FadingChannel(int c){int result=command(MX_FADING,c,0);return result<0?MIX_NO_FADING:(Mix_Fading)result;}
void SDLCALL Mix_ChannelFinished(void (SDLCALL *hook)(int)) {
    MX_Message message;int rounds;
    /* Registration starts a fresh notification interval. Discard events from
     * the previous hook, including time when no parent hook was installed. */
    if(opened&&belongs())for(rounds=0;rounds<128;rounds++) {
        SDL_memset(&message,0,sizeof(message));message.operation=MX_POLL;
        if(rpc(&message)<0||!message.length)break;
    }
    if(finished)SDL_memset(finished,0,(Uint32)finished_capacity*sizeof(*finished));
    finished_hook=hook;
}
Mix_Chunk * SDLCALL Mix_GetChunk(int c) {
    Sint32 id;MX_Message message;ClientChunk *entry;
    SDL_memset(&message,0,sizeof(message));message.operation=MX_GETCHUNK;message.arg[0]=c;
    id=rpc(&message);dispatch_finished();
    for(entry=chunks;entry;entry=entry->next)if(entry->session==session&&entry->id==(Uint32)id)return entry->chunk;
    return NULL;
}
static int unsupported_music(void){SDL_SetError("This PCM effects profile does not support music or compressed codecs");return -1;}
Mix_Music * SDLCALL Mix_LoadMUS(const char *p){(void)p;unsupported_music();return NULL;}
Mix_Music * SDLCALL Mix_LoadMUS_RW(SDL_RWops *p){(void)p;unsupported_music();return NULL;}
int SDLCALL Mix_PlayMusic(Mix_Music *m,int loops){(void)m;(void)loops;return unsupported_music();}
int SDLCALL Mix_FadeInMusic(Mix_Music *m,int loops,Sint32 ms){(void)m;(void)loops;(void)ms;return unsupported_music();}
int SDLCALL Mix_HaltMusic(void){return unsupported_music();}
int SDLCALL Mix_PlayingMusic(void){return 0;}
void SDLCALL Mix_FreeMusic(Mix_Music *m){(void)m;}
