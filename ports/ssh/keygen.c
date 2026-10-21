/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "keys.h"
#include "ripple_crypto.h"
#include <wolfssh/ssh.h>
#include <wolfssl/wolfcrypt/coding.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
static int public_file(const char *file,byte *blob,unsigned *length) {
    unsigned n,i,start,end;byte *data=ssh_read_file(file,&n,0);word32 out=SSH_BLOB_BYTES;int r=-1;
    if(!data)return -1;
    for(i=0;i<n&&data[i]!=' '&&data[i]!='\t';i++);
    if(i==n)goto done;start=i;while(start<n&&(data[start]==' '||data[start]=='\t'))start++;
    end=start;while(end<n&&data[end]!=' '&&data[end]!='\t'&&data[end]!='\n'&&data[end]!='\r')end++;
    if(!Base64_Decode(data+start,end-start,blob,&out)&&out>=4&&ssh_be32(blob)==i&&out>=i+4&&!memcmp(blob+4,data,i)){*length=out;r=0;}
 done:free(data);return r;
}
int main(int argc,char **argv) {
    struct ssh_key *key=calloc(1,sizeof(*key));char path[SSH_PATH],pubpath[SSH_PATH],line[SSH_PUBLIC_LINE],finger[80],pass[256]={0},confirm[256]={0},directory[SSH_PATH];
    const char *file=NULL,*newpass=NULL,*oldpass=NULL,*comment="riplix",*type="ecdsa";
    unsigned bits=0,kind=0,i,n;int export=0,list=0,change=0,r=1;
    if(!key)return 1;
    for(i=1;i<(unsigned)argc;i++) {
        if(!strcmp(argv[i],"-y"))export=1;else if(!strcmp(argv[i],"-l"))list=1;else if(!strcmp(argv[i],"-p"))change=1;
        else if(i+1<(unsigned)argc) {
            if(!strcmp(argv[i],"-f"))file=argv[++i];else if(!strcmp(argv[i],"-N"))newpass=argv[++i];
            else if(!strcmp(argv[i],"-P"))oldpass=argv[++i];else if(!strcmp(argv[i],"-C"))comment=argv[++i];
            else if(!strcmp(argv[i],"-t"))type=argv[++i];else if(!strcmp(argv[i],"-b")){char *end;unsigned long b=strtoul(argv[++i],&end,10);if(*end||(b!=256&&b!=384&&b!=521&&b!=2048&&b!=3072&&b!=4096))goto usage;bits=b;}
            else goto usage;
        } else goto usage;
    }
    kind=!strcmp(type,"ecdsa")?SSH_ECDSA:!strcmp(type,"rsa")?SSH_RSA:!strcmp(type,"ed25519")?SSH_ED25519:0;
    if(!kind||export+list+change>1)goto usage;
    if(!bits)bits=kind==SSH_RSA?3072:256;
    if(kind==SSH_ECDSA&&(bits!=256&&bits!=384&&bits!=521)||kind==SSH_RSA&&(bits!=2048&&bits!=3072&&bits!=4096)||kind==SSH_ED25519&&bits!=256)goto usage;
    if(!file){if(ssh_directory(directory,sizeof(directory))||ssh_path(path,sizeof(path),kind==SSH_RSA?"id_rsa":kind==SSH_ED25519?"id_ed25519":"id_ecdsa"))goto failed;file=path;}
    if(wolfSSH_Init()!=WS_SUCCESS)goto failed;ripple_crypto_init();
    if(list) {
        if(public_file(file,key->blob,&key->blob_length)&&ssh_key_load(key,file,oldpass))goto failed;
        if(ssh_fingerprint(key->blob,key->blob_length,finger,sizeof(finger)))goto failed;
        puts(finger);r=0;goto done;
    }
    if(export||change) {
        if(ssh_key_load(key,file,oldpass))goto failed;
        if(export){n=ssh_public_line(key->blob,key->blob_length,"",line,sizeof(line));if((int)n<0)goto failed;fputs(line,stdout);r=0;goto done;}
    } else {if(kind==SSH_RSA){fprintf(stderr,"Generating RSA-%u key; software generation may take a while.\n",bits);fflush(stderr);}if(ssh_key_generate(key,kind,bits))goto failed;}
    if(!newpass) {
        if(ssh_secret("New passphrase (empty for no passphrase): ",pass,sizeof(pass))||ssh_secret("Repeat passphrase: ",confirm,sizeof(confirm)))goto failed;
        if(strcmp(pass,confirm)){fputs("ssh-keygen: passphrases do not match\n",stderr);goto done;}newpass=pass;
    }
    if(strlen(newpass)>255){errno=EOVERFLOW;goto failed;}
    if(change){if(ssh_key_save(key,file,newpass,1))goto failed;puts("Passphrase updated.");r=0;goto done;}
    if(snprintf(pubpath,sizeof(pubpath),"%s.pub",file)>=sizeof(pubpath)){errno=ENAMETOOLONG;goto failed;}
    n=ssh_public_line(key->blob,key->blob_length,comment,line,sizeof(line));if((int)n<0)goto failed;
    /* Reserve the public name first. Never overwrite an existing key pair. */
    if(ssh_write_file(pubpath,(byte *)line,n,0644,0))goto failed;
    if(ssh_key_save(key,file,newpass,0)){int saved=errno;unlink(pubpath);errno=saved;goto failed;}
    if(ssh_fingerprint(key->blob,key->blob_length,finger,sizeof(finger)))goto failed;
    printf("Private key: %s\nPublic key: %s\n%s\n",file,pubpath,finger);r=0;goto done;
failed:perror("ssh-keygen");goto done;
usage:fputs("usage: ssh-keygen [-t ecdsa|rsa|ed25519] [-b bits] [-f file] [-C comment] [-N passphrase]\n"
            "       ssh-keygen -y|-l|-p [-f file] [-P old-passphrase] [-N new-passphrase]\n",stderr);r=2;
done:ssh_wipe(key,sizeof(*key));free(key);ssh_wipe(pass,sizeof(pass));ssh_wipe(confirm,sizeof(confirm));return r;
}
