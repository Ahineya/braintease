/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Shared bootstrap policy for local, graphical and remote root sessions.
 * RH_OPEN/RH_READ/RH_CLOSE/RH_CHDIR select ordinary or hosted syscalls. */
#ifndef RIPPLE_ROOT_HOME_H
#define RIPPLE_ROOT_HOME_H
static char root_home[256] = "/root";
static char root_home_environment[261] = "HOME=/root";

static int root_home_record(char *line) {
    char *field[7], *p=line; unsigned i;
    field[0]=p;
    for(i=1;i<7;i++) {
        p=strchr(p,':'); if(!p)return 0;
        *p++=0; field[i]=p;
    }
    if(strcmp(field[0],"root") || strcmp(field[2],"0") ||
       field[5][0]!='/' || strlen(field[5])>=sizeof(root_home))return 0;
    /* The old factory passwd entry used /. An unupgraded image without /root
     * falls back below, keeping its existing settings accessible. */
    strcpy(root_home,strcmp(field[5],"/") ? field[5] : "/root");
    return 1;
}

static int root_home_start(void) {
    char line[512], block[128]; unsigned have=0,i; int fd,done=0,skip=0,result=0;
    long n;
    fd=RH_OPEN("/etc/passwd");
    if(fd>=0) {
        while(!done && (n=RH_READ(fd,block,sizeof(block)))>0) {
            for(i=0;i<(unsigned)n;i++) {
                if(block[i]=='\n') {
                    line[have]=0;
                    if(!skip && root_home_record(line)){done=1;break;}
                    have=0;skip=0;
                } else if(have<sizeof(line)-1)line[have++]=block[i];
                else skip=1;
            }
        }
        if(!done && !skip && have){line[have]=0;root_home_record(line);}
        RH_CLOSE(fd);
    }
    if(RH_CHDIR(root_home)<0) {strcpy(root_home,"/");result=-1;}
    strcpy(root_home_environment,"HOME=");strcat(root_home_environment,root_home);
    return result;
}
#endif
