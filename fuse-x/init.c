/* SPDX-License-Identifier: GPL-2.0-only */
#include "user.h"
#include <gx-panel.h>
#define RH_OPEN(p) fx_open(p,O_RDONLY,0)
#define RH_READ(fd,p,n) fx_read(fd,p,n)
#define RH_CLOSE(fd) fx_close(fd)
#define RH_CHDIR(p) fx_call(10,(unsigned long)(p),0,0)
#include "root-home.h"

extern unsigned fuse_headless, fuse_telnet, fuse_one_shot, fuse_stopping, fuse_console_closed;
extern unsigned fuse_stop_tasks(unsigned);
extern unsigned fuse_gui, fuse_rescue;
extern unsigned fuse_new_terminals;
static int graphics_service, desktop_shell, services_manager, console_started;
static int boot_file(const char *path){
    int fd=fx_open(path,O_RDONLY,0);if(fd<0)return 0;fx_close(fd);return 1;
}
static int desktop_available(void){
    struct gx_panel panel={0};int fd,result;
    if(fuse_rescue)return 0;
    fd=fx_open("/dev/gfx",O_RDWR,0);if(fd<0)return 0;
    panel.version=GXP_VERSION;
    result=fx_call(29,fd,GX_PANEL,(unsigned long)&panel);fx_close(fd);
    return result==0&&(panel.flags&GXP_CAP_OVERLAY);
}
static int start_services(void){
    int pid,fd;char *arguments[]={"svcd","--boot",NULL};
    char *environment[]={"PATH=/usr/bin:/bin","HOME=/","SHELL=/bin/sh","LANG=C",NULL};
    if(fuse_rescue||!boot_file("/etc/boot/services.ini"))return 0;
    pid=fx_fork();if(pid)return pid;
    /* Service output never shares the interactive console. */
    fd=fx_open("/dev/null",O_RDONLY,0);
    if(fd<0||fx_call(36,fd,0,0)<0)fx_exit(127);if(fd>2)fx_close(fd);
    fd=fx_open("/var/log/svcd.log",O_WRONLY|O_CREAT|O_APPEND,0600);
    if(fd<0||fx_call(36,fd,1,0)<0||fx_call(36,fd,2,0)<0)fx_exit(127);if(fd>2)fx_close(fd);
    fx_call(23,(unsigned long)"/bin/svcd",(unsigned long)arguments,(unsigned long)environment);
    fx_error("init svcd");fx_exit(127);return -1;
}

static int start_program(unsigned console) {
    int first=console==1&&!console_started&&!fuse_rescue&&boot_file("/etc/boot/console.ini");
    int pid=fx_fork(), group;
    char *arguments[4];
    char *environment[10];
    if(pid){if(pid>0&&console==1)console_started=1;return pid;}
    fx_call(35,SIGINT,0,0); fx_call(35,SIGQUIT,0,0);
    fx_call(35,SIGTTOU,1,0);
    if(console==3) {if(fx_call(78,0,0,0)<0)fx_error("init graphics setsid");}
    else if(fx_call(77,0,0,0)<0) fx_error("init setpgid");
    group=fx_call(18,0,0,0);
    if(console==1 && fx_call(29,0,TIOCSPGRP,(unsigned long)&group)<0) fx_error("init foreground group");
    fx_call(35,SIGTTOU,0,0);
    arguments[0]=console==3 ? "nano-X" : console==2 ? "gterm" : console ? "sh" : "telnetd";
    arguments[1]=console==3 ? "-p" : NULL;arguments[2]=arguments[3]=NULL;
    environment[0]="PATH=/usr/bin:/bin";
    environment[1]=root_home_environment;
    environment[2]="TERM=vt100";
    environment[3]="SHELL=/bin/sh";
    environment[4]="PS1=fuse-x# ";
    environment[5]="EDITOR=/bin/vi"; environment[6]="USER=root";
    environment[7]="LOGNAME=root"; environment[8]=environment[9]=NULL;
    if(console==4){
        arguments[0]="fusedeshell";
        environment[8]=boot_file("/usr/share/applications") ?
            "FUSEDE_APPLICATIONS=/usr/share/applications" : "FUSEDE_APPLICATIONS=/mnt/host/fusede/applications";
        if(boot_file("/bin/fusedeshell"))
            fx_call(23,(unsigned long)"/bin/fusedeshell",(unsigned long)arguments,(unsigned long)environment);
        else if(boot_file("/mnt/host/fusedeshell.fxe"))
            fx_call(23,(unsigned long)"/mnt/host/fusedeshell.fxe",(unsigned long)arguments,(unsigned long)environment);
        fx_puts("Fuse-X: Graphite shell unavailable; starting graphical terminal\n");
        arguments[0]="gterm";
        fx_call(23,(unsigned long)"/bin/gterm",(unsigned long)arguments,(unsigned long)environment);
        fx_error("init graphical fallback");fx_exit(127);
    }
    if(first){
        arguments[0]="console-session";arguments[1]="--config";arguments[2]="/etc/boot/console.ini";
        fx_call(23,(unsigned long)"/bin/console-session",(unsigned long)arguments,(unsigned long)environment);
        fx_error("init console-session; using shell");
        arguments[0]="sh";arguments[1]=arguments[2]=NULL;
    }
    fx_call(23,(unsigned long)(console==3 ? "/bin/nano-X" : console==2 ? "/bin/gterm" : console ? "/bin/sh" : "/bin/telnetd"),
            (unsigned long)arguments,(unsigned long)environment);
    fx_error(arguments[0]); fx_exit(127);
    return -1;
}

static int reap(int console) {
    int pid,status;
    while((pid=fx_call(55,-1UL,(unsigned long)&status,WNOHANG))>0) {
        if(pid==services_manager){
            services_manager=0;
            if(!fuse_stopping){fx_puts("Fuse-X: service supervisor stopped; console remains available (status ");fx_number(status);fx_puts(")\n");}
        }
        if(pid==desktop_shell){
            desktop_shell=0;
            if(!fuse_stopping&&status){fx_puts("Fuse-X: Graphite shell stopped; fallback controls remain available\n");start_program(2);}
        }
        if(pid==graphics_service && !fuse_stopping) {
            fx_puts("Fuse-X: Nano-X service stopped (status ");fx_number(status);fx_puts(")\n");
            graphics_service=0;
        }
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
    if(root_home_start()<0)fx_puts("init: home unavailable; starting in /\n");
    fx_call(35,SIGINT,1,0); fx_call(35,SIGQUIT,1,0);
    services_manager=start_services();
    if(services_manager<0){fx_error("init service supervisor fork");services_manager=0;}
    if(fuse_rescue)fx_puts("Fuse-X: rescue mode; boot services and console policy skipped\n");
    if(fuse_telnet && start_program(0)<0) fx_error("init telnetd fork");
    if(fuse_gui && (graphics_service=start_program(3))<0) fx_error("init graphics service fork");
    if(fuse_gui){
        if(desktop_available()) {if((desktop_shell=start_program(4))<0)fx_error("init Graphite shell fork");}
        else if(start_program(2)<0)fx_error("init graphical terminal fork");
    }
    if(!fuse_headless) {
        console=start_program(1);
        if(console<0) {fx_error("init shell fork");console=0;}
    }
    while(!fuse_stopping) {
        int previous=console;
        if(fuse_new_terminals) {
            --fuse_new_terminals;
            if(start_program(2)<0)fx_error("new terminal fork");
        }
        console=reap(console);
        if(previous && !console && fuse_one_shot) break;
        if(!console && !fuse_headless && !fuse_console_closed) {
            console=start_program(1);
            if(console<0) {fx_error("init shell fork");console=0;}
        }
        fx_call(37,1,0,0);
    }
    fuse_stopping=1;
    fx_puts("Fuse-X: stopping guest processes\n");
    /* Only notify the supervisor first: signalling every service at once
     * would bypass its dependency shutdown ordering. Overall watchdog: 90 s. */
    if(services_manager){
        fx_call(39,services_manager,SIGTERM,0);fx_call(39,services_manager,SIGCONT,0);
        for(attempts=0;services_manager&&attempts<900;attempts++){reap(0);if(services_manager)fx_call(37,1,0,0);}
        if(services_manager)fx_puts("Fuse-X: service shutdown deadline exceeded; forcing cleanup\n");
    }
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
    fx_puts("Hello from native Riplix! PID "); fx_number(fx_call(18,0,0,0));
    for (i=1;i<args->argc;i++) { fx_puts(" "); fx_puts(args->argv[i]); }
    fx_puts("\n"); fx_exit(0);
}
