/* SPDX-License-Identifier: MIT */
#include "internal.h"
static int private_file(const char *path) {
    struct stat before,after;int fd;
    if(lstat(path,&before)<0||!S_ISREG(before.st_mode)||(before.st_mode&077))return -1;
    fd=open(path,O_RDONLY);if(fd<0)return -1;
    if(fstat(fd,&after)<0||after.st_dev!=before.st_dev||after.st_ino!=before.st_ino||!S_ISREG(after.st_mode)||(after.st_mode&077)) {close(fd);return -1;}
    return fd;
}
int tg_config(TgClient *c,const char *path) {
    FILE *f;int fd,have_id=0,have_hash=0;char line[128],*end;unsigned lines=0,i;unsigned long n;
    fd=private_file(path);if(fd<0)return 0;f=fdopen(fd,"r");if(!f){close(fd);return 0;}
    while(fgets(line,sizeof(line),f)) {
        if(++lines>16||(!strchr(line,'\n')&&!feof(f)))goto fail;
        for(i=strlen(line);i&&((line[i-1]=='\n')||(line[i-1]=='\r'));i--)line[i-1]=0;
        if(!strncmp(line,"api_id=",7)) {
            if(have_id++||line[7]<'0'||line[7]>'9')goto fail;n=strtoul(line+7,&end,10);
            if(*end||!n||n>2147483647UL)goto fail;c->api_id=n;
        }else if(!strncmp(line,"api_hash=",9)) {
            if(have_hash++||strlen(line+9)!=32)goto fail;
            for(i=9;i<41;i++)if(!((line[i]>='a'&&line[i]<='f')||(line[i]>='A'&&line[i]<='F')||(line[i]>='0'&&line[i]<='9')))goto fail;
            memcpy(c->api_hash,line+9,33);
        }else if(line[0]&&line[0]!='#')goto fail;
    }
    i=!ferror(f)&&have_id==1&&have_hash==1;tg_erase(line,sizeof(line));fclose(f);return i;
fail:tg_erase(line,sizeof(line));fclose(f);return 0;
}
/* Fixed, checksummed, private record. A checksum detects corruption; it does
 * not replace filesystem access protection or encrypt the authorization key. */
#define SESSION_SIZE 328
int tg_load(TgClient *c) {
    TgByte record[SESSION_SIZE],hash[32];int fd;struct stat st;unsigned at=0;long n;
    if(lstat(c->session_path,&st)<0)return errno==ENOENT;
    fd=private_file(c->session_path);if(fd<0)return 0;
    if(st.st_size!=SESSION_SIZE){close(fd);return 0;}
    while(at<SESSION_SIZE){n=read(fd,record+at,SESSION_SIZE-at);if(n<0&&errno==EINTR)continue;if(n<=0){close(fd);tg_erase(record,sizeof(record));return 0;}at+=n;}close(fd);
    tg_hash(0,record,296,hash);
    if(memcmp(record,"FTG1",4)||!tg_equal(hash,record+296,32)||tg_le32(record+4)!=c->api_id||record[8]<1||record[8]>5||record[9]>1||record[10]!=1||record[11]){tg_erase(record,sizeof(record));return 0;}
    c->dc=record[8];c->test=record[9];memcpy(c->auth,record+12,256);memcpy(c->salt,record+268,8);
    c->time_offset=(int32_t)tg_le32(record+276);c->user_id=tg_le64(record+280);
    for(at=288;at<296;at++)if(record[at]){tg_erase(record,sizeof(record));return 0;}
    c->has_key=1;c->authorized=1;tg_erase(record,sizeof(record));return 1;
}
int tg_store(TgClient *c) {
    TgByte record[SESSION_SIZE],suffix[4];char temporary[280];int fd,saved;unsigned at=0;long n;
    if(!c->authorized||!c->has_key)return 0;
    if(!tg_random(suffix,4)||snprintf(temporary,sizeof(temporary),"%s.tmp.%u.%08lx",c->session_path,(unsigned)getpid(),(unsigned long)tg_le32(suffix))<0)return 0;
    memset(record,0,sizeof(record));memcpy(record,"FTG1",4);tg_put32(record+4,c->api_id);record[8]=c->dc;record[9]=c->test;record[10]=1;
    memcpy(record+12,c->auth,256);memcpy(record+268,c->salt,8);tg_put32(record+276,c->time_offset);tg_put64(record+280,c->user_id);tg_hash(0,record,296,record+296);
    fd=open(temporary,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0){tg_erase(record,sizeof(record));return 0;}
    while(at<SESSION_SIZE){n=write(fd,record+at,SESSION_SIZE-at);if(n<0&&errno==EINTR)continue;if(n<=0)break;at+=n;}
    saved=close(fd);tg_erase(record,sizeof(record));
    if(at!=SESSION_SIZE||saved<0||rename(temporary,c->session_path)<0){unlink(temporary);return 0;}return 1;
}
