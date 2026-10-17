/* SPDX-License-Identifier: GPL-2.0-only */
#include "user.h"

extern unsigned fuse_headless, fuse_telnet, fuse_one_shot, fuse_stopping, fuse_console_closed;
extern unsigned fuse_stop_tasks(unsigned);

static int start_program(unsigned console) {
    int pid=fx_fork(), group;
    char *arguments[2];
    char *environment[6];
    if(pid) return pid;
    fx_call(35,SIGINT,0,0); fx_call(35,SIGQUIT,0,0);
    fx_call(35,SIGTTOU,1,0);
    if(fx_call(77,0,0,0)<0) fx_error("init setpgid");
    group=fx_call(18,0,0,0);
    if(console && fx_call(29,0,TIOCSPGRP,(unsigned long)&group)<0) fx_error("init foreground group");
    fx_call(35,SIGTTOU,0,0);
    arguments[0]=console ? "sh" : "telnetd"; arguments[1]=NULL;
    environment[0]="PATH=/bin:/usr/bin";
    environment[1]="HOME=/";
    environment[2]="TERM=vt100";
    environment[3]="SHELL=/bin/sh";
    environment[4]="PS1=fuse-x# "; environment[5]=NULL;
    fx_call(23,(unsigned long)(console ? "/bin/sh" : "/bin/telnetd"),
            (unsigned long)arguments,(unsigned long)environment);
    fx_error(arguments[0]); fx_exit(127);
    return -1;
}

static int reap(int console) {
    int pid,status;
    while((pid=fx_call(55,-1UL,(unsigned long)&status,WNOHANG))>0) {
        if(pid==console) {
            fx_puts("Fuse-X: shell stopped\n");
            console=0;
        }
    }
    return console;
}

void fuse_init_entry(unsigned long argument) {
    int console=0;
    unsigned attempts;
    used(argument);
    if (fx_open("/dev/tty1",O_RDWR,0)!=0 || fx_call(17,0,0,0)!=1 || fx_call(17,0,0,0)!=2)
        fx_shutdown();
    fx_puts("Fuse-X: native /init running as PID 1\n");
    fx_call(35,SIGINT,1,0); fx_call(35,SIGQUIT,1,0);
    if(fuse_telnet && start_program(0)<0) fx_error("init telnetd fork");
    if(!fuse_headless) {
        console=start_program(1);
        if(console<0) {fx_error("init shell fork");console=0;}
    }
    while(!fuse_stopping) {
        int previous=console;
        console=reap(console);
        if(previous && !console && fuse_one_shot) break;
        if(!console && !fuse_headless && !fuse_console_closed) {
            console=start_program(1);
            if(console<0) {fx_error("init shell fork");console=0;}
        }
        fx_call(37,1,0,0);
    }
    fx_puts("Fuse-X: stopping guest processes\n");
    fuse_stop_tasks(SIGTERM);
    for(attempts=0;attempts<5 && fuse_stop_tasks(0);attempts++) {
        reap(0); fx_call(37,1,0,0);
    }
    fuse_stop_tasks(SIGKILL);
    for(attempts=0;attempts<20 && fuse_stop_tasks(0);attempts++) {
        reap(0); fx_call(37,1,0,0);
    }
    reap(0);
    if(fuse_stop_tasks(0)) {
        fx_puts("Fuse-X: guest processes did not stop\n");
        for(;;) fx_call(37,1,0,0);
    }
    fx_shutdown();
}

void fuse_hello_entry(unsigned long argument) {
    struct fx_arguments *args=(void *)argument;
    unsigned i;
    fx_puts("Hello from native FUZIX! PID "); fx_number(fx_call(18,0,0,0));
    for (i=1;i<args->argc;i++) { fx_puts(" "); fx_puts(args->argv[i]); }
    fx_puts("\n"); fx_exit(0);
}
