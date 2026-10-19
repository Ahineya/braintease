/*  A very simple, lite, version of tar for the Fuzix project

    Copyright(c) 2015 Brett M Gordon

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.


    NOTES:
    * hardlinks are added as normal files

    TODO:
    * remove stdio.h gunk
*/

/* Ripple adaptation: native-character records, bounded USTAR paths, streaming
 * input, checked I/O and numeric fields, and preservation of file metadata. */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <getopt.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <utime.h>

struct header {
    char name[100], mode[8], uid[8], gid[8], size[12], mtime[12], cksum[8];
    char type, lname[100], ustar[6], version[2], uname[32], gname[32];
    char major[8], minor[8], prefix[155], pad[12];
};
static struct header h;
static char buffer[512], member[257];
static char *rec, *recptr, *recend;
static unsigned recz = 512;
static int infile, outfile, verbose, noreplace, check_sum = 1, failed;
static char key, *ofile;
static struct stat astat;
struct dirfix {
    struct dirfix *next;
    unsigned mode, uid, gid;
    uint32_t mtime;
    char name[257];
};
static struct dirfix *directories;

static void fatal(const char *message) {
    fprintf(stderr, "tar: %s\n", message);
    exit(1);
}
static void problem(const char *name) {
    fprintf(stderr, "tar: %s: ", name);
    perror("");
    failed = 1;
}
static void writeall(int fd, const char *p, unsigned n) {
    while (n) {
        long r = write(fd, p, n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            problem(key == 'x' ? member : ofile ? ofile : "stdout");
            exit(1);
        }
        p += r;
        n -= r;
    }
}
static void blkread(void *buf) {
    if (recptr == recend) {
        unsigned n = 0;
        while (n < recz) {
            long r = read(infile, rec+n, recz-n);
            if (r < 0 && errno == EINTR)
                continue;
            if (r < 0) { problem(ofile ? ofile : "stdin"); exit(1); }
            if (!r)
                break;
            n += r;
        }
        if (!n || n % 512)
            fatal("truncated archive");
        recptr = rec;
        recend = rec+n;
    }
    memcpy(buf, recptr, 512);
    recptr += 512;
}
static void blkwrite(const void *buf) {
    memcpy(recptr, buf, 512);
    recptr += 512;
    if (recptr == recend) {
        writeall(outfile, rec, recz);
        recptr = rec;
    }
}
static void blkflush(void) {
    if (recptr != rec) {
        memset(recptr, 0, recend-recptr);
        writeall(outfile, rec, recz);
        recptr = rec;
    }
}
static uint32_t octal(const char *p, unsigned n) {
    uint32_t value = 0;
    while (n && *p == ' ') { p++; n--; }
    while (n && *p >= '0' && *p <= '7') {
        unsigned digit = *p++ - '0';
        if (value > (0xffffffffUL-digit)/8)
            fatal("numeric field too large");
        value = value*8+digit;
        n--;
    }
    while (n--) {
        if (*p && *p != ' ')
            fatal("invalid octal header field");
        p++;
    }
    return value;
}
static void putoctal(uint32_t value, char *p, unsigned n) {
    memset(p, '0', n);
    p[--n] = 0;
    while (value && n) {
        p[--n] = '0' + (value & 7);
        value >>= 3;
    }
    if (value)
        fatal("numeric field does not fit header");
}
static uint32_t checksum(void) {
    const unsigned char *p = (const unsigned char *)&h;
    uint32_t sum = 0;
    unsigned i;
    for (i = 0; i < 512; i++)
        sum += i >= 148 && i < 156 ? 32 : p[i];
    return sum;
}
/* Reject parent traversal and absolute extraction member names. Parent
 * components are checked separately so extraction never follows a symlink. */
static int safe_path(const char *p) {
    const char *start;
    if (!*p || *p == '/')
        return 0;
    while (*p) {
        start = p;
        while (*p && *p != '/') p++;
        if (p-start == 2 && start[0] == '.' && start[1] == '.')
            return 0;
        if (*p) p++;
    }
    return 1;
}
static unsigned fieldlen(const char *p, unsigned max) {
    unsigned n=0;
    while (n<max && p[n]) n++;
    return n;
}
static void readname(void) {
    unsigned prefix=0, name=fieldlen(h.name,100), n=0;
    if (!strncmp(h.ustar,"ustar",5))
        prefix=fieldlen(h.prefix,155);
    if (prefix) {
        memcpy(member,h.prefix,prefix);
        n=prefix;
        member[n++]='/';
    }
    memcpy(member+n,h.name,name);
    n+=name;
    member[n]=0;
    while (n>1 && member[n-1]=='/') member[--n]=0;
}
static int putname(const char *name) {
    unsigned n=strlen(name), i;
    if (n>256) return 0;
    if (n<=100) { memcpy(h.name,name,n); return 1; }
    for (i=n; i; i--)
        if (name[i-1]=='/' && i-1<=155 && n-i<=100 && n-i) {
            memcpy(h.prefix,name,i-1);
            memcpy(h.name,name+i,n-i);
            return 1;
        }
    return 0;
}
static void printheader(void) {
    if (key=='c' && !ofile) return;
    if (key=='t' && verbose)
        printf("%c %04lo %lu/%lu %lu ", h.type=='5'?'d':'-',
               octal(h.mode,8),octal(h.uid,8),octal(h.gid,8),octal(h.size,12));
    if (key=='t' || verbose)
        printf("%s\n",member);
}
static void metadata(void) {
    struct utimbuf times;
    if (octal(h.uid,8)>65535UL || octal(h.gid,8)>65535UL) {
        fprintf(stderr,"tar: owner id out of range: %s\n",member);
        failed=1;
        return;
    }
    if (chown(member,octal(h.uid,8),octal(h.gid,8))<0)
        problem(member);
    if (chmod(member,octal(h.mode,8)&07777)<0)
        problem(member);
    times.actime=times.modtime=octal(h.mtime,12);
    if (utime(member,&times)<0)
        problem(member);
}
static void defer_directory(void) {
    struct dirfix *d=malloc(sizeof(*d));
    if (!d) fatal("out of memory for directory metadata");
    if (octal(h.uid,8)>65535UL || octal(h.gid,8)>65535UL)
        fatal("directory owner id out of range");
    strcpy(d->name,member);
    d->mode=octal(h.mode,8); d->uid=octal(h.uid,8); d->gid=octal(h.gid,8);
    d->mtime=octal(h.mtime,12);
    d->next=directories; directories=d;
}
static void finish_directories(void) {
    while (directories) {
        struct dirfix *d=directories;
        directories=d->next;
        strcpy(member,d->name);
        putoctal(d->mode,h.mode,8); putoctal(d->uid,h.uid,8);
        putoctal(d->gid,h.gid,8); putoctal(d->mtime,h.mtime,12);
        metadata();
        free(d);
    }
}
static void store(const char *name, unsigned depth) {
    struct stat s;
    unsigned long remaining;
    int fd=-1;
    const char *aname=name;
    if (depth>128) fatal("directory nesting too deep");
    if (lstat(name,&s)<0) { problem(name); return; }
    if (s.st_dev==astat.st_dev && s.st_ino==astat.st_ino) return;
    while (*aname=='/') aname++;
    if (!*aname) aname=".";
    if (!safe_path(aname)) { fprintf(stderr,"tar: unsafe name: %s\n",name); failed=1; return; }
    memset(&h,0,sizeof(h));
    if (!putname(aname)) { fprintf(stderr,"tar: name too long: %s\n",name); failed=1; return; }
    if (S_ISREG(s.st_mode)) {
        fd=open(name,O_RDONLY);
        if (fd<0) { problem(name); return; }
        h.type='0';
    } else if (S_ISLNK(s.st_mode)) {
        int n;
        if (s.st_size > sizeof(h.lname)) {
            fprintf(stderr,"tar: link target too long for USTAR: %s\n",name);failed=1;return;
        }
        n=readlink(name,h.lname,sizeof(h.lname));
        if (n<0) { problem(name);return; }
        h.type='2';
    } else if (S_ISDIR(s.st_mode)) h.type='5';
    else if (S_ISCHR(s.st_mode)) h.type='3';
    else if (S_ISBLK(s.st_mode)) h.type='4';
    else if (S_ISFIFO(s.st_mode)) h.type='6';
    else { fprintf(stderr,"tar: unsupported file: %s\n",name); failed=1; return; }
    putoctal(s.st_mode&07777,h.mode,8);
    putoctal(s.st_uid,h.uid,8); putoctal(s.st_gid,h.gid,8);
    putoctal(S_ISREG(s.st_mode)?s.st_size:0,h.size,12);
    putoctal(s.st_mtime,h.mtime,12);
    putoctal(major(s.st_rdev),h.major,8); putoctal(minor(s.st_rdev),h.minor,8);
    memcpy(h.ustar,"ustar",5); memcpy(h.version,"00",2);
    putoctal(checksum(),h.cksum,7); h.cksum[7]=' ';
    readname(); printheader(); blkwrite(&h);
    if (fd>=0) {
        remaining=s.st_size;
        while (remaining) {
            unsigned want=remaining>512?512:remaining, n=0;
            memset(buffer,0,512);
            while (n<want) {
                long r=read(fd,buffer+n,want-n);
                if (r<0 && errno==EINTR) continue;
                if (r<=0) fatal("source file changed or could not be read");
                n+=r;
            }
            blkwrite(buffer);
            remaining-=want;
        }
        if (close(fd)<0) problem(name);
    } else if (S_ISDIR(s.st_mode)) {
        DIR *dir=opendir(name);
        struct dirent *entry;
        char child[258];
        if (!dir) { problem(name); return; }
        for (;;) {
            off_t position;
            errno=0;
            entry=readdir(dir);
            if (!entry) { if (errno) problem(name); break; }
            if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
            if (strlen(name)+strlen(entry->d_name)+2>sizeof(child)) {
                failed=1; fprintf(stderr,"tar: pathname too long\n"); continue;
            }
            strcpy(child,name); strcat(child,"/"); strcat(child,entry->d_name);
            position=telldir(dir); closedir(dir);
            store(child,depth+1);
            dir=opendir(name);
            if (!dir) { problem(name); break; }
            seekdir(dir,position);
        }
        if (dir) closedir(dir);
    }
}
static void skipdata(uint32_t size) {
    while (size) { blkread(buffer); size=size>512?size-512:0; }
}
static int selected(char **args) {
    if (!*args) return 1;
    while (*args) {
        unsigned n=strlen(*args);
        while (n && (*args)[n-1]=='/') n--;
        if (!strncmp(member,*args,n) && (!member[n] || member[n]=='/')) return 1;
        args++;
    }
    return 0;
}
static int parents(const char *name) {
    char path[257];
    unsigned i;
    strcpy(path,name);
    for (i=1; path[i]; i++)
        if (path[i]=='/') {
            struct stat s;
            path[i]=0;
            if (mkdir(path,0755)<0 && (errno!=EEXIST || lstat(path,&s)<0 || !S_ISDIR(s.st_mode))) {
                problem(path); return 0;
            }
            path[i]='/';
        }
    return 1;
}
static void unpack(char **args) {
    unsigned zeros=0;
    for (;;) {
        uint32_t size;
        unsigned i;
        int empty=1, fd=-1;
        blkread(&h);
        for (i=0;i<512;i++) if (((char *)&h)[i]) { empty=0; break; }
        if (empty) { if (++zeros==2) break; continue; }
        zeros=0;
        if (check_sum && checksum()!=octal(h.cksum,8)) fatal("bad header checksum");
        if (h.type=='x' || h.type=='g' || h.type=='L' || h.type=='K')
            fatal("GNU/PAX extended headers are not supported");
        size=octal(h.size,12);
        readname();
        if (!selected(args)) { skipdata(size); continue; }
        printheader();
        if (key=='t') { skipdata(size); continue; }
        if (!safe_path(member)) {
            fprintf(stderr,"tar: unsafe name: %s\n",member); failed=1; skipdata(size); continue;
        }
        if (!parents(member)) { skipdata(size); continue; }
        if (h.type=='0' || !h.type) {
            struct stat existing;
            /* Never truncate the referent of an existing symbolic link. */
            if (lstat(member,&existing)==0 && S_ISLNK(existing.st_mode)) {
                if (noreplace || unlink(member)<0) {
                    if(noreplace)errno=EEXIST;
                    problem(member);skipdata(size);continue;
                }
            }
            int flags=O_CREAT|O_WRONLY|(noreplace?O_EXCL:O_TRUNC);
            fd=open(member,flags,0600);
            if (fd<0) { problem(member); skipdata(size); continue; }
            while (size) {
                unsigned n=size>512?512:size;
                blkread(buffer);
                writeall(fd,buffer,n);
                size-=n;
            }
            if (close(fd)<0) problem(member);
            metadata();
            continue;
        }
        if (h.type=='5') {
            struct stat s;
            if (mkdir(member,0700)<0 && (errno!=EEXIST || lstat(member,&s)<0 || !S_ISDIR(s.st_mode)))
                problem(member);
            else defer_directory();
        } else if (h.type=='2') {
            char target[101];
            memcpy(target,h.lname,100); target[100]=0;
            if (symlink(target,member)<0) problem(member);
            /* chmod/chown/utime follow links; leave link metadata alone. */
        } else if (h.type=='1') {
            char target[101];
            memcpy(target,h.lname,100); target[100]=0;
            if (!safe_path(target)) { failed=1; fprintf(stderr,"tar: unsafe link target\n"); }
            else if (!parents(target)) failed=1;
            else if (lstat(target,&astat)<0 || !S_ISREG(astat.st_mode)) {
                fprintf(stderr,"tar: hard link target must be a regular file: %s\n",target);failed=1;
            }
            else if (link(target,member)<0) problem(member);
            else metadata();
        } else if (h.type=='3' || h.type=='4' || h.type=='6') {
            unsigned type=h.type=='3'?S_IFCHR:h.type=='4'?S_IFBLK:S_IFIFO;
            if (octal(h.major,8)>255 || octal(h.minor,8)>255) {
                failed=1; fprintf(stderr,"tar: device number out of range\n");
            } else if (mknod(member,(octal(h.mode,8)&07777)|type,
                       makedev(octal(h.major,8),octal(h.minor,8)))<0) problem(member);
            else metadata();
        } else { fprintf(stderr,"tar: unsupported member type: %c\n",h.type); failed=1; }
        skipdata(size);
    }
}
int main(int argc, char **argv) {
    int option;
    unsigned bf=1;
    char *traditional=NULL;
    ofile=getenv("TAPE");
    if (argc>1 && argv[1][0]!='-') {
        traditional=malloc(strlen(argv[1])+2);
        if (!traditional) fatal("out of memory");
        traditional[0]='-'; strcpy(traditional+1,argv[1]); argv[1]=traditional;
    }
    while ((option=getopt(argc,argv,"ctxvknf:b:"))!=-1) {
        switch (option) {
        case 'c': case 't': case 'x':
            if (key && key!=option) fatal("choose one of c, t, x");
            key=option; break;
        case 'v': verbose=1; break;
        case 'k': noreplace=1; break;
        case 'n': check_sum=0; break;
        case 'f': ofile=optarg; break;
        case 'b': {
            char *end;
            long n=strtol(optarg,&end,10);
            if (*end || n<1 || n>32) fatal("blocking factor must be 1..32");
            bf=n; break;
        }
        default: fatal("usage: tar -c|-t|-x [-vk] [-b factor] [-f archive] [path ...]");
        }
    }
    if (!key) fatal("choose one of c, t, x");
    if (ofile && !strcmp(ofile,"-")) ofile=NULL;
    recz=bf*512;
    rec=malloc(recz);
    if (!rec) fatal("out of memory");
    recptr=rec; recend=rec+recz;
    if (key=='c') {
        outfile=ofile?open(ofile,O_WRONLY|O_CREAT|O_TRUNC,0666):1;
        if (outfile<0) { problem(ofile); return 1; }
        if (fstat(outfile,&astat)<0) { problem("archive"); return 1; }
        while (optind<argc) store(argv[optind++],0);
        memset(buffer,0,512); blkwrite(buffer); blkwrite(buffer); blkflush();
        if (close(outfile)<0) problem(ofile?ofile:"stdout");
    } else {
        infile=ofile?open(ofile,O_RDONLY):0;
        if (infile<0) { problem(ofile); return 1; }
        recptr=recend;
        unpack(argv+optind);
        if (key=='x') finish_directories();
        close(infile);
    }
    free(traditional); free(rec);
    return failed;
}
