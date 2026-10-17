/* SPDX-License-Identifier: GPL-2.0-only */
#include "user.h"

static unsigned split(char *line, char **argv) {
    unsigned n=0;
    char quote, *write=line;
    while (*line) {
        while (*line==' ' || *line=='\t' || *line=='\n' || *line=='\r') line++;
        if (!*line || n==15) break;
        argv[n++]=write;
        quote=0;
        while (*line) {
            char c=*line++;
            if ((c=='\'' || c=='"') && (!quote || quote==c)) { quote=quote ? 0 : c; continue; }
            if (!quote && (c==' ' || c=='\t' || c=='\n' || c=='\r')) break;
            if (c=='\\' && *line) c=*line++;
            *write++=c;
        }
        *write++=0;
    }
    argv[n]=NULL;
    return n;
}
void fuse_shell_entry(unsigned long argument) {
    fx_call(35,SIGINT,1,0); fx_call(35,SIGQUIT,1,0);
    char line[256], path[256], *argv[16];
    long n; unsigned argc, i;
    used(argument);
    fx_puts("Fuse-X native shell. Type help for commands.\n");
    for (;;) {
        fx_puts("fuse-x# ");
        n=fx_read(0,line,sizeof(line)-1);
        if (n<=0) { fx_puts("\n"); fx_exit(0); }
        line[n]=0; argc=split(line,argv);
        if (!argc) continue;
        if (!strcmp(argv[0],"exit")) fx_exit(0);
        else if (!strcmp(argv[0],"shutdown")) fx_shutdown();
        else if (!strcmp(argv[0],"help"))
            fx_puts("help, echo, ls [path], cat path, cd path, write path text, sleep [seconds], pid, hello [args], check, exit, shutdown\n"
                    "Use a full path to run another registered native program. Pipelines and redirection are not implemented.\n");
        else if (!strcmp(argv[0],"echo")) {
            for(i=1;i<argc;i++) { if(i>1) fx_puts(" "); fx_puts(argv[i]); } fx_puts("\n");
        } else if (!strcmp(argv[0],"pid")) { fx_number(fx_call(18,0,0,0)); fx_puts("\n"); }
        else if (!strcmp(argv[0],"cd") && argc>1) {
            if (fx_call(10,(unsigned long)argv[1],0,0)<0) fx_error(argv[1]);
        } else if (!strcmp(argv[0],"write") && argc>2) {
            int fd=fx_open(argv[1],O_WRONLY|O_CREAT|O_TRUNC,0644);
            if (fd<0) fx_error(argv[1]);
            else {
                for(i=2;i<argc;i++) { if(i>2) fx_write(fd," ",1); fx_write(fd,argv[i],strlen(argv[i])); }
                fx_write(fd,"\n",1); fx_close(fd);
            }
        } else if (!strcmp(argv[0],"sleep")) {
            unsigned seconds=0;
            if (argc>1) for(i=0;argv[1][i]>='0' && argv[1][i]<='9';i++) seconds=seconds*10+argv[1][i]-'0';
            if (seconds && seconds<3276) fx_call(37,seconds*10,0,0);
        } else {
            if (strlen(argv[0])>240) { fx_puts("command too long\n"); continue; }
            if (strchr(argv[0],'/')) strcpy(path,argv[0]);
            else { strcpy(path,"/bin/"); strcat(path,argv[0]); }
            fx_run(path,argv);
        }
    }
}
