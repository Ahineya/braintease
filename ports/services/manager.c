/* SPDX-License-Identifier: MIT */
#include "service.h"
static struct services *all;
static const char *runtime;
static int listener=-1,peer=-1,stopping;
static char request[SV_INPUT],response[SV_REPLY];
static unsigned requested,sending,sent;
static int status_pending;
static size_t status_index,status_only;
static unsigned long peer_deadline;
static volatile int interrupted;
static const char *states[]={"down","waiting","starting","running","done","stopping","backoff","failed","blocked"};
static unsigned long now_ms(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (unsigned long)t.tv_sec*1000UL+t.tv_nsec/1000000UL;}
static int due(unsigned long now,unsigned long deadline){return (long)(now-deadline)>=0;}
static void signal_stop(int n){interrupted=n;signal(n,signal_stop);}
static void child_changed(int n){signal(n,child_changed);}
static int nonblock(int fd){int flags=fcntl(fd,F_GETFL,0);return flags<0?-1:fcntl(fd,F_SETFL,flags|O_NONBLOCK);}
static int cloexec(int fd){return fcntl(fd,F_SETFD,FD_CLOEXEC);}
static void close_channel(int *fd){if(*fd>=0)close(*fd);*fd=-1;}
static int ready(struct service *s){return !s->want_stop&&(s->state==SV_RUNNING||s->state==SV_DONE);}
static void group_signal(struct service *s,int sig){if(s->pid>0){if(kill(-s->pid,sig)<0&&errno==ESRCH)kill(s->pid,sig);}}
static void stop_service(struct service *s,unsigned long now){
    if(s->pid>0){if(s->state!=SV_STOPPING){group_signal(s,SIGTERM);group_signal(s,SIGCONT);s->state=SV_STOPPING;s->deadline=now+s->stop_ms;}}
    else s->state=s->desired?SV_WAITING:SV_DOWN;
}
static void failed(struct service *s,unsigned long now){
    if(s->desired&&s->restart!=SV_NO&&s->retries<s->restart_limit){s->retries++;s->state=SV_BACKOFF;s->deadline=now+s->restart_ms;}
    else s->state=SV_FAILED;
}
static void child_error(int fd){int saved=errno;write(fd,&saved,sizeof(saved));_exit(127);}
static void launch(struct service *s,unsigned long now){
    int errors[2],notification[2]={-1,-1},pid,i,logfd,nullfd;char path[512],ready_env[40];
    char *argv[SV_ARGS+2],*env[]={"PATH=/bin:/usr/bin","HOME=/","SHELL=/bin/sh","LANG=C",NULL,NULL};
    if(pipe(errors)){s->last_exit=127;failed(s,now);return;}
    if(cloexec(errors[0])||cloexec(errors[1])||nonblock(errors[0]))goto pipe_failed;
    if(s->notify){if(pipe(notification))goto pipe_failed;if(cloexec(notification[0])||nonblock(notification[0]))goto pipe_failed;}
    snprintf(path,sizeof(path),"%s/%s.log",runtime,s->name);
    pid=fork();if(pid<0)goto pipe_failed;
    if(!pid){
        signal(SIGINT,SIG_DFL);signal(SIGQUIT,SIG_DFL);signal(SIGTERM,SIG_DFL);signal(SIGHUP,SIG_DFL);signal(SIGPIPE,SIG_DFL);signal(SIGCHLD,SIG_DFL);
        close(errors[0]);if(notification[0]>=0)close(notification[0]);
        if(setsid()<0||chdir(s->directory))child_error(errors[1]);
        nullfd=open("/dev/null",O_RDONLY);if(nullfd<0||dup2(nullfd,0)<0)child_error(errors[1]);if(nullfd>2)close(nullfd);
        logfd=open(path,O_WRONLY|O_CREAT|O_APPEND,0600);
        if(logfd<0||dup2(logfd,1)<0||dup2(logfd,2)<0)child_error(errors[1]);if(logfd>2)close(logfd);
        if(s->notify){snprintf(ready_env,sizeof(ready_env),"SVC_READY_FD=%d",notification[1]);env[4]=ready_env;}
        argv[0]=s->program;for(i=0;i<(int)s->argc;i++)argv[i+1]=s->args[i];argv[s->argc+1]=NULL;
        execve(s->program,argv,env);child_error(errors[1]);
    }
    close(errors[1]);if(notification[1]>=0)close(notification[1]);
    s->pid=pid;s->exec_fd=errors[0];s->ready_fd=notification[0];s->exec_done=s->notified=0;s->state=SV_STARTING;s->deadline=now+s->start_ms;s->last_exit=s->last_signal=0;
    return;
pipe_failed:
    close(errors[0]);close(errors[1]);if(notification[0]>=0)close(notification[0]);if(notification[1]>=0)close(notification[1]);s->last_exit=127;failed(s,now);
}
static void channels(struct service *,unsigned long);
static void reap(unsigned long now){
    int pid,status;size_t i;
    while((pid=waitpid(-1,&status,WNOHANG))>0)for(i=0;i<all->count;i++)if(all->item[i].pid==pid){
        struct service *s=&all->item[i];unsigned old;
        if(s->state==SV_STARTING)channels(s,now);old=s->state;
        /* Foreground service contract: clean up descendants that remain in its
         * process group. Deliberately detached descendants need daemon support. */
        kill(-s->pid,SIGKILL);s->pid=0;close_channel(&s->exec_fd);close_channel(&s->ready_fd);
        s->last_exit=WIFEXITED(status)?WEXITSTATUS(status):0;s->last_signal=WIFSIGNALED(status)?WTERMSIG(status):0;
        if(old==SV_STOPPING||s->want_stop)s->state=s->desired?SV_WAITING:SV_DOWN;
        else if(s->last_exit||s->last_signal||old==SV_STARTING&&!s->exec_done||s->notify&&old!=SV_RUNNING)failed(s,now);
        else if(s->oneshot)s->state=SV_DONE;
        else if(s->restart==SV_ALWAYS)failed(s,now);
        else{s->state=SV_DOWN;s->desired=0;}
        s->want_stop=0;break;
    }
}
static void channels(struct service *s,unsigned long now){
    int result,error;char message[16];
    if(s->exec_fd>=0){
        result=read(s->exec_fd,&error,sizeof(error));
        if(!result){s->exec_done=1;close_channel(&s->exec_fd);if(!s->notify&&!s->oneshot)s->state=SV_RUNNING;}
        else if(result>0){fprintf(stderr,"svcd: %s: exec failed: %s\n",s->name,strerror(error));close_channel(&s->exec_fd);}
    }
    if(s->ready_fd>=0){
        result=read(s->ready_fd,message,sizeof(message));
        if(result>0){
            /* A single R byte is the complete bounded readiness protocol. */
            if(result==1&&message[0]=='R'){s->notified=1;close_channel(&s->ready_fd);}
            else{close_channel(&s->ready_fd);s->deadline=now;}
        }else if(!result){close_channel(&s->ready_fd);s->deadline=now;}
    }
    if(s->state==SV_STARTING&&s->exec_done&&s->notify&&s->notified)s->state=SV_RUNNING;
    if(s->state==SV_STARTING&&due(now,s->deadline)){
        fprintf(stderr,"svcd: %s: startup/readiness timed out\n",s->name);s->last_signal=SIGKILL;group_signal(s,SIGKILL);
    }
}
static int dependencies(struct service *s){
    size_t j;int wait=0;
    for(j=0;j<s->needs.count;j++){
        struct service *other=&all->item[s->needs.item[j]];
        if(!ready(other)){if(other->state==SV_FAILED||other->state==SV_BLOCKED||!other->desired)return -1;wait=1;}
    }
    for(j=0;j<s->before.count;j++){
        struct service *other=&all->item[s->before.item[j]];
        if(other->desired&&(other->state==SV_WAITING||other->state==SV_STARTING||other->state==SV_BACKOFF))wait=1;
    }
    return wait?0:1;
}
static void tick(unsigned long now){
    size_t i,j;unsigned starting=0;int dep,dependent;
    /* Startup notifications must be read before reaping fast successful tasks. */
    for(i=0;i<all->count;i++)if(all->item[i].pid&&all->item[i].state==SV_STARTING)channels(&all->item[i],now);
    reap(now);
    for(i=all->count;i>0;i--){
        struct service *s=&all->item[all->order[i-1]];
        if(s->pid&&(!s->desired||dependencies(s)<1&&(s->state==SV_RUNNING||s->state==SV_STARTING)))s->want_stop=1;
        if(s->want_stop&&s->pid){
            dependent=0;
            for(j=0;j<all->count;j++)if(all->item[j].pid&&(sv_has(&all->item[j].needs,all->order[i-1])||
                ((!all->item[j].desired||all->item[j].want_stop)&&sv_has(&all->item[j].before,all->order[i-1])))){dependent=1;break;}
            if(!dependent)stop_service(s,now);
        }
        if(s->state==SV_STOPPING&&due(now,s->deadline))group_signal(s,SIGKILL);
    }
    for(i=0;i<all->count;i++)if(all->item[i].state==SV_STARTING)starting++;
    for(i=0;i<all->count;i++){
        struct service *s=&all->item[all->order[i]];
        if(s->state==SV_BACKOFF&&due(now,s->deadline))s->state=SV_WAITING;
        if(s->desired&&(s->state==SV_DOWN||s->state==SV_WAITING||s->state==SV_BLOCKED)){
            dep=dependencies(s);s->state=dep<0?SV_BLOCKED:SV_WAITING;
            if(dep>0&&starting<SV_STARTUPS){launch(s,now);if(s->state==SV_STARTING)starting++;}
        }
    }
}
static void stop_set(size_t index){
    size_t i,j;
    if(!all->count)return;
    memset(all->marks,index==SV_NONE?1:0,all->count);
    if(index!=SV_NONE)all->marks[index]=1;
    /* Providers precede consumers, so one forward pass finds the stop closure. */
    for(i=0;i<all->count;i++){
        size_t at=all->order[i];struct service *s=&all->item[at];
        for(j=0;j<s->needs.count;j++)if(all->marks[s->needs.item[j]]){all->marks[at]=1;break;}
        if(all->marks[at]){s->desired=0;s->want_stop=s->pid>0;if(!s->pid)s->state=SV_DOWN;}
    }
}
static void reply(const char *text){status_pending=0;snprintf(response,sizeof(response),"%s\n",text);sending=strlen(response);sent=0;}
/* Stream one status row at a time: reply memory is independent of service count. */
static int next_status(void){
    struct service *s;
    if(!status_pending)return 0;
    if(status_only!=SV_NONE){if(status_index){status_pending=0;return 0;}s=&all->item[status_only];status_index=1;}
    else{if(status_index==all->count){status_pending=0;return 0;}s=&all->item[status_index++];}
    sending=snprintf(response,sizeof(response),"%s %s pid=%d wanted=%u retries=%u exit=%d signal=%d\n",s->name,states[s->state],s->pid,s->desired,s->retries,s->last_exit,s->last_signal);sent=0;return 1;
}
static void activate(size_t index){
    size_t i;sv_select(all,index);
    for(i=0;i<all->count;i++)if(all->marks[i]){
        struct service *s=&all->item[i];s->desired=1;
        if(s->state==SV_FAILED||s->state==SV_DOWN||s->state==SV_BLOCKED){s->state=SV_WAITING;s->retries=0;}
    }
}
static void control(void){
    char *verb,*name,*extra;size_t index=SV_NONE;
    if(memchr(request,0,requested)||memchr(request,'\n',requested)!=request+requested-1){reply("ERR one text command per connection required");return;}
    request[requested]=0;verb=strtok(request," \t\r\n");name=strtok(NULL," \t\r\n");extra=strtok(NULL," \t\r\n");
    if(!verb||extra||(name&&(index=sv_find(all,name))==SV_NONE)){reply("ERR invalid request or unknown service");return;}
    if(!strcmp(verb,"status")){
        reply("OK");status_pending=1;status_index=0;status_only=index;return;
    }
    if(!strcmp(verb,"shutdown")&&!name){stopping=1;stop_set(SV_NONE);reply("OK stopping supervisor");return;}
    if(!name||stopping){reply("ERR service name required, or supervisor stopping");return;}
    if(!strcmp(verb,"start")){activate(index);reply("OK start requested");}
    else if(!strcmp(verb,"restart")){activate(index);all->item[index].retries=0;all->item[index].want_stop=all->item[index].pid>0;if(!all->item[index].pid)all->item[index].state=SV_WAITING;reply("OK restart requested");}
    else if(!strcmp(verb,"stop")){stop_set(index);reply("OK stop requested");}
    else reply("ERR unknown command");
}
static void close_peer(void){if(peer>=0)close(peer);peer=-1;requested=sending=sent=0;status_pending=0;}
static int loop(void){
    size_t i;int fd,maxfd,n,live;fd_set reads,writes;struct timeval timeout;
    for(;;){
        unsigned long now=now_ms();
        if(interrupted&&!stopping){stopping=1;stop_set(SV_NONE);}
        tick(now);live=0;for(i=0;i<all->count;i++)if(all->item[i].pid)live=1;
        if(stopping&&!live&&peer<0)return 0;
        if(peer>=0&&due(now,peer_deadline))close_peer();
        FD_ZERO(&reads);FD_ZERO(&writes);maxfd=listener;
        if(!stopping)FD_SET(listener,&reads);
        if(peer>=0){if(sending)FD_SET(peer,&writes);else FD_SET(peer,&reads);if(peer>maxfd)maxfd=peer;}
        for(i=0;i<all->count;i++){struct service *s=&all->item[i];fd=s->exec_fd;if(fd>=0){FD_SET(fd,&reads);if(fd>maxfd)maxfd=fd;}fd=s->ready_fd;if(fd>=0){FD_SET(fd,&reads);if(fd>maxfd)maxfd=fd;}}
        timeout.tv_sec=0;timeout.tv_usec=100000;n=select(maxfd+1,&reads,&writes,NULL,&timeout);
        if(n<0){if(errno==EINTR)continue;perror("svcd select");interrupted=1;continue;}
        if(peer>=0){
            if(sending&&FD_ISSET(peer,&writes)){n=write(peer,response+sent,sending-sent);if(n>0){sent+=n;peer_deadline=now_ms()+2000;if(sent==sending&&!next_status())close_peer();}else if(!n||errno!=EINTR&&errno!=EAGAIN)close_peer();}
            else if(!sending&&FD_ISSET(peer,&reads)){
                n=read(peer,request+requested,sizeof(request)-1-requested);
                if(n>0){requested+=n;if(memchr(request,'\n',requested))control();else if(requested==sizeof(request)-1)reply("ERR request too long");}
                else if(!n||errno!=EINTR&&errno!=EAGAIN)close_peer();
            }
        }
        if(!stopping&&FD_ISSET(listener,&reads)){
            fd=accept(listener,NULL,NULL);
            if(fd>=0){if(peer>=0||nonblock(fd)||cloexec(fd))close(fd);else{peer=fd;peer_deadline=now_ms()+2000;requested=sending=sent=0;}}
        }
    }
}
int main(int argc,char **argv){
    const char *directory=NULL,*profile=NULL;size_t at;int check=0,boot=0,i,result=1;char error[512];struct sockaddr_un address;
    runtime=NULL;
    for(i=1;i<argc;i++){
        if(!strcmp(argv[i],"--check"))check=1;
        else if(!strcmp(argv[i],"--boot"))boot=1;
        else if(i+1<argc&&!strcmp(argv[i],"--config"))directory=argv[++i];
        else if(i+1<argc&&!strcmp(argv[i],"--runtime"))runtime=argv[++i];
        else if(i+1<argc&&!strcmp(argv[i],"--profile"))profile=argv[++i];else goto usage;
    }
    if(boot){
        if(argc!=2||getppid()!=1){fputs("svcd: --boot is reserved for the initial PID-1 launch\n",stderr);return 2;}
        directory="/etc/services";profile="/etc/boot/services.ini";runtime="/var/run/svcd";
        if(setsid()<0){perror("svcd boot setsid");return 1;}
    }
    if(!directory||!check&&!runtime)goto usage;
    all=calloc(1,sizeof(*all));if(!all){perror("svcd memory");return 1;}
    if(sv_load(all,directory,error,sizeof(error))||sv_profile(all,profile,error,sizeof(error))){fprintf(stderr,"svcd: %s\n",error);goto done;}
    if(check){for(at=0;at<all->count;at++){struct service *s=&all->item[all->order[at]];printf("%s %s\n",s->name,s->desired?"enabled":"disabled");}result=0;goto done;}
    memset(&address,0,sizeof(address));address.sun_family=AF_UNIX;
    if(sv_runtime(runtime,1,address.sun_path,sizeof(address.sun_path))){perror("svcd runtime");goto done;}
    if(boot){
        struct stat st;
        /* PID 1 launches this once before other managed services exist. Only
         * that fresh-boot path may remove a socket left on the persistent disk. */
        if(!lstat(address.sun_path,&st)){
            if(!S_ISSOCK(st.st_mode)||st.st_uid!=getuid()||unlink(address.sun_path)){fputs("svcd: unsafe stale boot socket\n",stderr);goto done;}
        }else if(errno!=ENOENT){perror("svcd boot socket");goto done;}
    }
    listener=socket(AF_UNIX,SOCK_STREAM,0);
    if(listener<0||nonblock(listener)||cloexec(listener)||bind(listener,(struct sockaddr *)&address,sizeof(address))||listen(listener,4)){perror("svcd socket (remove a stale socket only after confirming its manager stopped)");goto done;}
    signal(SIGPIPE,SIG_IGN);signal(SIGINT,signal_stop);signal(SIGTERM,signal_stop);signal(SIGCHLD,child_changed);
    puts("SVCD_READY (only explicitly enabled services start)");fflush(stdout);result=loop();
    close_peer();close(listener);listener=-1;unlink(address.sun_path);
done:
    if(listener>=0)close(listener);sv_free(all);free(all);return result;
usage:
    fputs("usage: svcd --boot (PID 1 only)\n       svcd --config DIR [--profile FILE] --runtime PRIVATE_DIR\n       svcd --check --config DIR [--profile FILE]\nNo profile means no automatic service starts. Foreground, opt-in manager.\n",stderr);return 2;
}
