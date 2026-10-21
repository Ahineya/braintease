/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "keys.h"
#include <wolfssh/internal.h>
#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/rsa.h>
#include <wolfssl/wolfcrypt/ed25519.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/oid_sum.h>
#include <wolfssl/wolfcrypt/coding.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <wolfssl/wolfcrypt/hmac.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <vm_crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <sys/stat.h>
#include <dirent.h>

void ssh_wipe(void *data,unsigned n){volatile byte *p=data;while(n--)*p++=0;}
word32 ssh_be32(const byte *p){return ((word32)p[0]<<24)|((word32)p[1]<<16)|((word32)p[2]<<8)|p[3];}
void ssh_put32(byte *p,word32 n){p[0]=(n>>24)&255;p[1]=(n>>16)&255;p[2]=(n>>8)&255;p[3]=n&255;}
static volatile int interrupted;
static void interrupt(int sig){interrupted=sig;signal(sig,interrupt);}
int ssh_secret(const char *prompt,char *out,unsigned cap) {
    int fd,n,r=-1,have=0,overflow=0;unsigned at=0;char ch;struct termios old,t;
    void (*onint)(int),(*onterm)(int),(*onquit)(int),(*onhup)(int);
    fd=open("/dev/tty",O_RDWR);if(fd<0)return -1;
    if(tcgetattr(fd,&old)<0){close(fd);return -1;}
    t=old;t.c_lflag&=~(ECHO|ECHONL);t.c_lflag|=ICANON|ISIG;
    interrupted=0;onint=signal(SIGINT,interrupt);onterm=signal(SIGTERM,interrupt);
    onquit=signal(SIGQUIT,interrupt);onhup=signal(SIGHUP,interrupt);
    if(tcsetattr(fd,TCSANOW,&t)<0)goto done;
    have=1;if(write(fd,prompt,strlen(prompt))<0)goto done;
    while(!interrupted) {
        n=read(fd,&ch,1);if(n<0&&errno==EINTR)continue;if(n!=1)goto done;
        if(ch=='\n'||ch=='\r'){if(overflow)errno=EOVERFLOW;else {out[at]=0;r=0;}break;}
        /* Drain an overlong secret before restoring echo: its remaining
         * characters must never become the caller shell's next command. */
        if(ch>255||at+1>=cap){overflow=1;continue;}
        if(!overflow)out[at++]=ch;
    }
done:
    if(have){if(tcsetattr(fd,TCSANOW,&old)<0)r=-1;write(fd,"\n",1);}
    signal(SIGINT,onint);signal(SIGTERM,onterm);signal(SIGQUIT,onquit);signal(SIGHUP,onhup);close(fd);
    if(r)ssh_wipe(out,cap);if(interrupted)errno=EINTR;return r;
}
int ssh_path(char *out,unsigned cap,const char *name) {
    const char *home=getenv("HOME");int n;
    if(!home||home[0]!='/')home="/root";
    n=snprintf(out,cap,"%s/.ssh%s%s",home,*name?"/":"",name);
    if(n<0||(unsigned)n>=cap){errno=ENAMETOOLONG;return -1;}return 0;
}
int ssh_directory(char *out,unsigned cap) {
    struct stat st;
    if(ssh_path(out,cap,""))return -1;
    if(lstat(out,&st)<0) {if(errno!=ENOENT||mkdir(out,0700)<0)return -1;if(lstat(out,&st)<0)return -1;}
    if(!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&022)){errno=EACCES;return -1;}
    return 0;
}
static int name_lengths(const char *path) {
    unsigned i,n=0;
    for(i=0;;i++) {
        if(!path[i]||path[i]=='/'){if(n>NAME_MAX){errno=ENAMETOOLONG;return -1;}n=0;if(!path[i])return 0;}
        else n++;
        if(i>=SSH_PATH-1){errno=ENAMETOOLONG;return -1;}
    }
}
byte *ssh_read_file(const char *path,unsigned *length,int private) {
    struct stat a,b;int fd;unsigned at=0,i;ssize_t n;byte *data;
    if(name_lengths(path)||lstat(path,&a)<0)return NULL;
    if(!S_ISREG(a.st_mode)||a.st_uid!=geteuid()||(a.st_mode&(private?077:022))){errno=EACCES;return NULL;}
    if(a.st_size<0||a.st_size>16384){errno=EFBIG;return NULL;}
    fd=open(path,O_RDONLY);if(fd<0)return NULL;
    if(fstat(fd,&b)<0||a.st_ino!=b.st_ino||a.st_dev!=b.st_dev||a.st_uid!=b.st_uid||a.st_mode!=b.st_mode||a.st_size!=b.st_size){close(fd);errno=EACCES;return NULL;}
    data=malloc((unsigned)a.st_size+1);if(!data){close(fd);return NULL;}
    while(at<(unsigned)a.st_size) {
        n=read(fd,data+at,(unsigned)a.st_size-at);if(n<0&&errno==EINTR)continue;
        if(n<=0)goto bad;
        for(i=0;i<(unsigned)n;i++)if(data[at+i]>255){errno=EINVAL;goto bad;}
        at+=n;
    }
    if(close(fd)<0){fd=-1;goto bad;}
    data[at]=0;*length=at;return data;
bad:
    if(fd>=0)close(fd);ssh_wipe(data,(unsigned)a.st_size+1);free(data);return NULL;
}
int ssh_write_file(const char *path,const byte *data,unsigned length,unsigned mode,int replace) {
    char tmp[SSH_PATH];const char *target=path;struct stat before,after;int fd,n,saved=0;unsigned at=0;
    if(name_lengths(path))return -1;
    if(replace) {
        if(lstat(path,&before)<0||!S_ISREG(before.st_mode)||before.st_uid!=geteuid()||(before.st_mode&022)){errno=EACCES;return -1;}
        {static unsigned serial;const char *slash=strrchr(path,'/');unsigned parent=slash?(unsigned)(slash-path+1):0;
         if(parent+32>=sizeof(tmp)){errno=ENAMETOOLONG;return -1;}memcpy(tmp,path,parent);
         n=snprintf(tmp+parent,sizeof(tmp)-parent,".ssh-%u-%u",(unsigned)getpid(),++serial);
         if(n<0||(unsigned)n>=sizeof(tmp)-parent){errno=ENAMETOOLONG;return -1;}target=tmp;}
    }
    fd=open(target,O_WRONLY|O_CREAT|O_EXCL|O_SYNC,mode);if(fd<0)return -1;
    while(at<length) {n=write(fd,data+at,length-at);if(n<0&&errno==EINTR)continue;if(n<=0){saved=errno?errno:EIO;break;}at+=n;}
    if(close(fd)<0&&!saved)saved=errno;
    if(!saved&&replace) {
        if(lstat(path,&after)<0||after.st_ino!=before.st_ino||after.st_dev!=before.st_dev||after.st_uid!=before.st_uid||after.st_mode!=before.st_mode||after.st_size!=before.st_size)saved=EACCES;
        else if(rename(tmp,path)<0)saved=errno;
    }
    if(saved){unlink(target);errno=saved;return -1;}return 0;
}
static int public_key(struct ssh_key *k,ecc_key *key) {
    const char *curve;byte point[133];word32 n=sizeof(point);unsigned at,len;
    k->type=SSH_ECDSA;
    k->bits=wc_ecc_get_curve_size_from_id(wc_ecc_get_curve_id(key->idx))*8;
    if(k->bits==528)k->bits=521;
    if(k->bits==256)curve="nistp256";else if(k->bits==384)curve="nistp384";else if(k->bits==521)curve="nistp521";else return -1;
    snprintf(k->algorithm,sizeof(k->algorithm),"ecdsa-sha2-%s",curve);
    if(wc_ecc_export_x963(key,point,&n))return -1;
    len=strlen(k->algorithm);ssh_put32(k->blob,len);memcpy(k->blob+4,k->algorithm,len);at=4+len;
    len=strlen(curve);ssh_put32(k->blob+at,len);memcpy(k->blob+at+4,curve,len);at+=4+len;
    ssh_put32(k->blob+at,n);memcpy(k->blob+at+4,point,n);k->blob_length=at+4+n;return 0;
}
/* RFC 4251 positive mpint: prepend zero only when its sign bit is set. */
static unsigned mpint(byte *out,const byte *in,unsigned n) {
    unsigned zero=n&&(in[0]&128)?1:0;ssh_put32(out,n+zero);
    if(zero)out[4]=0;memcpy(out+4+zero,in,n);return 4+n+zero;
}
static int rsa_public(struct ssh_key *k,RsaKey *key) {
    byte exponent[8],modulus[512];word32 e=sizeof(exponent),n=sizeof(modulus);unsigned at=11;
    if(wc_RsaFlattenPublicKey(key,exponent,&e,modulus,&n)||n<256||n>512)return -1;
    k->type=SSH_RSA;k->bits=wc_RsaEncryptSize(key)*8;strcpy(k->algorithm,"ssh-rsa");
    ssh_put32(k->blob,7);memcpy(k->blob+4,"ssh-rsa",7);
    at+=mpint(k->blob+at,exponent,e);at+=mpint(k->blob+at,modulus,n);k->blob_length=at;return 0;
}
static int ed_public(struct ssh_key *k,ed25519_key *key) {
    word32 n=32;k->type=SSH_ED25519;k->bits=256;strcpy(k->algorithm,"ssh-ed25519");
    if(!key->pubKeySet&&wc_ed25519_make_public(key,key->p,32))return -1;
    ssh_put32(k->blob,11);memcpy(k->blob+4,"ssh-ed25519",11);ssh_put32(k->blob+15,32);
    if(wc_ed25519_export_public(key,k->blob+19,&n)||n!=32)return -1;k->blob_length=51;return 0;
}
int ssh_key_generate(struct ssh_key *k,unsigned type,unsigned bits) {
    WC_RNG rng;int r=-1;memset(k,0,sizeof(*k));
    if(wc_InitRng(&rng))return -1;
    if(type==SSH_ECDSA) {
        ecc_key *key=calloc(1,sizeof(*key));int id=bits==256?ECC_SECP256R1:bits==384?ECC_SECP384R1:bits==521?ECC_SECP521R1:0;
        if(key){if(id&&!wc_ecc_init(key)) {r=wc_ecc_make_key_ex(&rng,(bits+7)/8,key,id);
            if(!r)r=public_key(k,key);if(!r)r=wc_EccKeyToDer(key,k->der,sizeof(k->der));wc_ecc_free(key);}free(key);}
    } else if(type==SSH_RSA) {
        RsaKey *key=calloc(1,sizeof(*key));
        if(key){if((bits==2048||bits==3072||bits==4096)&&!wc_InitRsaKey(key,NULL)) {
            wc_RsaSetRNG(key,&rng);r=wc_MakeRsaKey(key,bits,65537L,&rng);
            if(!r)r=rsa_public(k,key);if(!r)r=wc_RsaKeyToDer(key,k->der,sizeof(k->der));wc_FreeRsaKey(key);}free(key);}
    } else if(type==SSH_ED25519&&bits==256) {
        ed25519_key key;if(!wc_ed25519_init(&key)) {r=wc_ed25519_make_key(&rng,32,&key);
            if(!r)r=ed_public(k,&key);if(!r)r=wc_Ed25519PrivateKeyToDer(&key,k->der,sizeof(k->der));wc_ed25519_free(&key);}
    }
    wc_FreeRng(&rng);if(r<=0){ssh_wipe(k,sizeof(*k));errno=EINVAL;return -1;}k->der_length=r;return 0;
}
/* Normalize accepted DER containers to the representation wolfSSH signs. */
static int decode_key(struct ssh_key *k) {
    word32 at=0;int r;ecc_key *ec=calloc(1,sizeof(*ec));RsaKey *rsa;ed25519_key ed;
    if(!ec)return -1;
    r=wc_ecc_init(ec);if(!r) {r=wc_EccPrivateKeyDecode(k->der,&at,ec,k->der_length);
        if(!r&&at==k->der_length&&!public_key(k,ec))r=wc_EccKeyToDer(ec,k->der,sizeof(k->der));else r=-1;wc_ecc_free(ec);}
    free(ec);if(r>0){k->der_length=r;return 0;}
    rsa=calloc(1,sizeof(*rsa));if(!rsa)return -1;at=0;
    r=wc_InitRsaKey(rsa,NULL);if(!r) {r=wc_RsaPrivateKeyDecode(k->der,&at,rsa,k->der_length);
        if(!r&&at==k->der_length&&!rsa_public(k,rsa))r=wc_RsaKeyToDer(rsa,k->der,sizeof(k->der));else r=-1;wc_FreeRsaKey(rsa);}
    free(rsa);if(r>0){k->der_length=r;return 0;}at=0;
    r=wc_ed25519_init(&ed);if(!r) {r=wc_Ed25519PrivateKeyDecode(k->der,&at,&ed,k->der_length);
        if(!r&&at==k->der_length&&!ed_public(k,&ed))r=wc_Ed25519PrivateKeyToDer(&ed,k->der,sizeof(k->der));else r=-1;wc_ed25519_free(&ed);}
    if(r>0){k->der_length=r;return 0;}return -1;
}
int ssh_public_line(const byte *blob,unsigned length,const char *comment,char *out,unsigned cap) {
    byte encoded[2048];word32 n=sizeof(encoded);unsigned type;int r;
    if(length<4||(type=ssh_be32(blob))>80||type>length-4)return -1;
    if(Base64_Encode_NoNl(blob,length,encoded,&n)||n>=sizeof(encoded))return -1;encoded[n]=0;
    if(comment&&(strchr(comment,'\n')||strchr(comment,'\r'))){errno=EINVAL;return -1;}
    r=snprintf(out,cap,"%.*s %s%s%s\n",(int)type,blob+4,encoded,comment&&*comment?" ":"",comment?comment:"");
    return r<0||(unsigned)r>=cap?-1:r;
}
int ssh_fingerprint(const byte *blob,unsigned length,char *out,unsigned cap) {
    byte hash[32],base64[64];word32 n=sizeof(base64);int r;
    if(wc_Sha256Hash(blob,length,hash)||Base64_Encode_NoNl(hash,sizeof(hash),base64,&n))return -1;
    while(n&&base64[n-1]=='=')n--;base64[n]=0;r=snprintf(out,cap,"SHA256:%s",base64);return r<0||(unsigned)r>=cap?-1:0;
}
/* PKCS#8 PBES2 envelope: PBKDF2-HMAC-SHA512 and AES-256-CBC. The DER reader
 * accepts only this bounded profile; crypto primitives remain wolfCrypt/device
 * operations. No private-file format or encryption algorithm is invented. */
struct der {byte data[SSH_KEY_BYTES+512];unsigned n;};
struct view {const byte *p;unsigned n;};
static const byte pbes2[]={0x2a,0x86,0x48,0x86,0xf7,0x0d,1,5,13};
static const byte pbkdf2[]={0x2a,0x86,0x48,0x86,0xf7,0x0d,1,5,12};
static const byte sha512[]={0x2a,0x86,0x48,0x86,0xf7,0x0d,2,11};
static const byte aes256[]={0x60,0x86,0x48,1,0x65,3,4,1,0x2a};
static int tlv(struct der *d,unsigned tag,const byte *data,unsigned n) {
    unsigned header=n<128?2:n<256?3:4;
    if(n>4000||d->n+header+n>sizeof(d->data))return -1;
    d->data[d->n++]=tag;if(n<128)d->data[d->n++]=n;
    else {d->data[d->n++]=n<256?0x81:0x82;if(n>=256)d->data[d->n++]=n>>8;d->data[d->n++]=n&255;}
    if(n)memcpy(d->data+d->n,data,n);d->n+=n;return 0;
}
static int wrap(struct der *out,unsigned tag,struct der *inner){return tlv(out,tag,inner->data,inner->n);}
static int take(struct view *v,unsigned tag,struct view *out) {
    unsigned n,count,i;if(v->n<2||v->p[0]!=tag)return -1;
    n=v->p[1];v->p+=2;v->n-=2;
    if(n&128){count=n&127;if(!count||count>2||count>v->n||!v->p[0])return -1;n=0;for(i=0;i<count;i++)n=(n<<8)|v->p[i];v->p+=count;v->n-=count;if(n<128)return -1;}
    if(n>v->n)return -1;out->p=v->p;out->n=n;v->p+=n;v->n-=n;return 0;
}
static int oid(struct view *v,const byte *p,unsigned n){struct view o;return take(v,6,&o)||o.n!=n||memcmp(o.p,p,n)?-1:0;}
static int derive(const char *password,const byte *salt,unsigned saltlen,word32 iterations,byte *out) {
    struct vm_crypto_request *p;int fd,r,saved;unsigned i,j,n=strlen(password);Hmac h;byte block[64]={0},u[64]={0},index[4]={0,0,0,1};word32 round;
    if(n>255||saltlen<8||saltlen>64||iterations<1000||iterations>1000000UL){errno=EINVAL;return -1;}
    p=calloc(1,sizeof(*p));if(!p)return -1;
    p->version=1;p->operation=VM_CRYPTO_PBKDF2_SHA512;p->a_len=n;p->b_len=saltlen;p->c_len=4;p->out_len=64;
    memcpy(p->a,password,n);memcpy(p->b,salt,saltlen);ssh_put32(p->c,iterations);
    fd=open("/dev/crypto",O_RDWR);r=-1;saved=ENOSYS;
    if(fd>=0){r=ioctl(fd,RIPPLE_CRYPTO_IOCTL,p);saved=errno;close(fd);}
    if(!r){if(p->out_len==64)memcpy(out,p->out,32);else {r=-1;saved=EIO;}}
    ssh_wipe(p,sizeof(*p));free(p);
    if(!r)return 0;
    if(saved!=ENOSYS&&saved!=ENOTTY&&fd>=0){errno=saved;return -1;}
    r=wc_HmacInit(&h,NULL,INVALID_DEVID);if(r)return -1;
    if(!r)r=wc_HmacSetKey(&h,WC_SHA512,(const byte *)password,n);
    if(!r)r=wc_HmacUpdate(&h,salt,saltlen);if(!r)r=wc_HmacUpdate(&h,index,4);if(!r)r=wc_HmacFinal(&h,u);
    memcpy(block,u,64);
    for(round=1;!r&&round<iterations;round++){r=wc_HmacUpdate(&h,u,64);if(!r)r=wc_HmacFinal(&h,u);if(!r)for(j=0;j<64;j++)block[j]^=u[j];}
    if(!r)for(i=0;i<32;i++)out[i]=block[i];
    wc_HmacFree(&h);ssh_wipe(block,64);ssh_wipe(u,64);return r?-1:0;
}
static int decrypt(byte *data,unsigned size,const char *password) {
    struct view v={data,size},outer,alg,params,kdf,kparams,enc,prf,salt,iv,cipher,it,unused;
    word32 iterations=0;byte key[32],pad,bad=0;Aes aes;unsigned i,n;int r=-1;
    if(take(&v,0x30,&outer)||v.n||take(&outer,0x30,&alg)||oid(&alg,pbes2,sizeof(pbes2))||take(&alg,0x30,&params)||alg.n||take(&params,0x30,&kdf)||oid(&kdf,pbkdf2,sizeof(pbkdf2))||take(&kdf,0x30,&kparams)||kdf.n||take(&kparams,4,&salt)||take(&kparams,2,&it)||!it.n||it.n>4||(it.p[0]&128))return -1;
    for(i=0;i<it.n;i++)iterations=(iterations<<8)|it.p[i];
    if(kparams.n&&kparams.p[0]==2){if(take(&kparams,2,&unused)||unused.n!=1||unused.p[0]!=32)return -1;}
    if(take(&kparams,0x30,&prf)||oid(&prf,sha512,sizeof(sha512)))return -1;
    if(prf.n&&(take(&prf,5,&unused)||unused.n))return -1;
    if(prf.n||kparams.n||take(&params,0x30,&enc)||oid(&enc,aes256,sizeof(aes256))||take(&enc,4,&iv)||iv.n!=16||enc.n||params.n||take(&outer,4,&cipher)||outer.n||!cipher.n||cipher.n%16||cipher.n>SSH_KEY_BYTES)return -1;
    if(derive(password,salt.p,salt.n,iterations,key))return -1;
    if(wc_AesInit(&aes,NULL,INVALID_DEVID)){ssh_wipe(key,32);return -1;}
    if(!wc_AesSetKey(&aes,key,32,iv.p,AES_DECRYPTION)&&!wc_AesCbcDecrypt(&aes,data,cipher.p,cipher.n)) {
        pad=data[cipher.n-1];bad=(!pad||pad>16);
        for(i=0;i<16;i++)if(i<pad)bad|=data[cipher.n-1-i]^pad;
        if(!bad){n=cipher.n-pad;r=n;}
    }
    wc_AesFree(&aes);ssh_wipe(key,32);return r;
}
static int encrypt(struct ssh_key *k,const char *password,byte *out,unsigned cap) {
    struct der *all=calloc(9,sizeof(*all)),*prf,*kp,*ks,*kd,*ea,*ps,*al,*body,*outer;
    byte plain[SSH_KEY_BYTES],cipher[SSH_KEY_BYTES],salt[16],iv[16],key[32],iter[]={3,0x0d,0x40};
    const byte *curve;word32 curvesize=0,n=sizeof(plain);ecc_key ecc;WC_RNG rng;Aes aes;unsigned padded,pad;int r=-1,have_aes=0;
    if(!all)return -1;
    prf=all;kp=all+1;ks=all+2;kd=all+3;ea=all+4;ps=all+5;al=all+6;body=all+7;outer=all+8;
    if(wc_ecc_init(&ecc)){free(all);return -1;}
    if(k->type==SSH_ECDSA) {
        word32 at=0;if(wc_EccPrivateKeyDecode(k->der,&at,&ecc,k->der_length))goto ecc_done;
        if(wc_ecc_get_oid(ecc.dp->oidSum,&curve,&curvesize)<0||wc_CreatePKCS8Key(plain,&n,k->der,k->der_length,ECDSAk,curve,curvesize)<0)goto ecc_done;
    } else if(k->type==SSH_RSA) {
        if(wc_CreatePKCS8Key(plain,&n,k->der,k->der_length,RSAk,NULL,0)<0)goto ecc_done;
    } else {n=k->der_length;memcpy(plain,k->der,n);}
    if(wc_InitRng(&rng))goto ecc_done;
    if(wc_RNG_GenerateBlock(&rng,salt,16)||wc_RNG_GenerateBlock(&rng,iv,16)||derive(password,salt,16,200000UL,key))goto done;
    pad=16-(n%16);padded=n+pad;if(padded>sizeof(plain))goto done;memset(plain+n,pad,pad);
    if(wc_AesInit(&aes,NULL,INVALID_DEVID))goto done;have_aes=1;
    if(wc_AesSetKey(&aes,key,32,iv,AES_ENCRYPTION)||wc_AesCbcEncrypt(&aes,cipher,plain,padded))goto done;
    if(tlv(prf,6,sha512,sizeof(sha512))||tlv(prf,5,NULL,0)||tlv(kp,4,salt,16)||tlv(kp,2,iter,3)||wrap(kp,0x30,prf)||wrap(ks,0x30,kp)||tlv(kd,6,pbkdf2,sizeof(pbkdf2)))goto done;
    memcpy(kd->data+kd->n,ks->data,ks->n);kd->n+=ks->n;
    if(tlv(ea,6,aes256,sizeof(aes256))||tlv(ea,4,iv,16)||wrap(ps,0x30,kd)||wrap(ps,0x30,ea)||tlv(al,6,pbes2,sizeof(pbes2))||wrap(al,0x30,ps)||wrap(body,0x30,al)||tlv(body,4,cipher,padded)||wrap(outer,0x30,body))goto done;
    r=wc_DerToPem(outer->data,outer->n,out,cap,PKCS8_ENC_PRIVATEKEY_TYPE);
done:
    if(have_aes)wc_AesFree(&aes);wc_FreeRng(&rng);
ecc_done:
    wc_ecc_free(&ecc);ssh_wipe(all,9*sizeof(*all));free(all);ssh_wipe(plain,sizeof(plain));ssh_wipe(cipher,sizeof(cipher));ssh_wipe(key,32);return r;
}
int ssh_key_save(struct ssh_key *k,const char *path,const char *pass,int replace) {
    byte *pem=malloc(8192);int n,r;if(!pem)return -1;
    n=*pass?encrypt(k,pass,pem,8192):wc_DerToPem(k->der,k->der_length,pem,8192,k->type==SSH_ECDSA?ECC_PRIVATEKEY_TYPE:k->type==SSH_RSA?PRIVATEKEY_TYPE:PKCS8_PRIVATEKEY_TYPE);
    r=n>0?ssh_write_file(path,pem,n,0600,replace):-1;ssh_wipe(pem,8192);free(pem);if(n<=0)errno=EINVAL;return r;
}
/* OpenSSH's unencrypted container. The bcrypt-encrypted variant is rejected;
 * encrypted imports use the supported PBES2 PKCS#8 profile instead. */
static int ssh_string(const byte *p,unsigned size,unsigned *at,const byte **out,unsigned *n) {
    word32 length;if(*at>size||size-*at<4)return -1;length=ssh_be32(p+*at);*at+=4;
    if(length>size-*at)return -1;*out=p+*at;*n=length;*at+=length;return 0;
}
static int decode_openssh(struct ssh_key *k) {
    const byte *p=k->der,*value,*pub;unsigned at=15,n,publen;word32 idx=0;
    WS_KeySignature *key;int r=-1;
    if(k->der_length<15||memcmp(p,"openssh-key-v1",15))return -1;
    if(ssh_string(p,k->der_length,&at,&value,&n)||n!=4||memcmp(value,"none",4)||
       ssh_string(p,k->der_length,&at,&value,&n)||n!=4||memcmp(value,"none",4)||
       ssh_string(p,k->der_length,&at,&value,&n)||n||at+4>k->der_length||ssh_be32(p+at)!=1)return -1;
    at+=4;
    if(ssh_string(p,k->der_length,&at,&pub,&publen)||publen>SSH_BLOB_BYTES||
       ssh_string(p,k->der_length,&at,&value,&n)||n%8||at!=k->der_length)return -1;
    /* Save the outer public blob before replacing the private container. */
    byte expected[SSH_BLOB_BYTES];memcpy(expected,pub,publen);
    key=calloc(1,sizeof(*key));if(!key)return -1;
    if(GetOpenSshKey(key,p,k->der_length,&idx)==WS_SUCCESS&&idx==k->der_length) {
        if(key->keyId==ID_SSH_RSA) {r=rsa_public(k,&key->ks.rsa.key);if(!r)r=wc_RsaKeyToDer(&key->ks.rsa.key,k->der,sizeof(k->der));}
        else if(key->keyId==ID_ED25519) {r=ed_public(k,&key->ks.ed25519.key);if(!r)r=wc_Ed25519PrivateKeyToDer(&key->ks.ed25519.key,k->der,sizeof(k->der));}
        else if(key->keyId==ID_ECDSA_SHA2_NISTP256||key->keyId==ID_ECDSA_SHA2_NISTP384||key->keyId==ID_ECDSA_SHA2_NISTP521) {
            r=public_key(k,&key->ks.ecc.key);if(!r)r=wc_EccKeyToDer(&key->ks.ecc.key,k->der,sizeof(k->der));
        }
    }
    wolfSSH_KEY_clean(key);free(key);
    if(r<=0||publen!=k->blob_length||memcmp(expected,k->blob,publen))return -1;k->der_length=r;return 0;
}
int ssh_key_load(struct ssh_key *k,const char *path,const char *pass) {
    byte *data,*decoded=NULL;unsigned size,i,at=0;word32 n;char password[256];int encrypted=0,r=-1;
    memset(k,0,sizeof(*k));memset(password,0,sizeof(password));data=ssh_read_file(path,&size,1);if(!data)return -1;
    if(size>10&&!memcmp(data,"-----BEGIN",10)) {
        char *start=strchr((char *)data,'\n'),*end=strstr((char *)data,"-----END ");
        encrypted=strstr((char *)data,"-----BEGIN ENCRYPTED PRIVATE KEY-----")== (char *)data;
        if(!start||!end||end<=start)goto done;
        decoded=malloc(size);if(!decoded)goto done;
        for(i=start-(char *)data+1;i<(unsigned)(end-(char *)data);i++)if(data[i]!='\n'&&data[i]!='\r')decoded[at++]=data[i];
        n=sizeof(k->der);r=Base64_Decode(decoded,at,k->der,&n);if(r){fprintf(stderr,"ssh: invalid key PEM encoding (%d)\n",r);goto done;}k->der_length=n;
    } else {if(size>sizeof(k->der))goto done;memcpy(k->der,data,size);k->der_length=size;}
    if(encrypted) {
        if(!pass){if(ssh_secret("Enter key passphrase: ",password,sizeof(password)))goto done;pass=password;}
        r=decrypt(k->der,k->der_length,pass);if(r<=0)goto done;k->der_length=r;
    }
    r=k->der_length>=15&&!memcmp(k->der,"openssh-key-v1",15)?decode_openssh(k):decode_key(k);
    if(r)fputs("ssh: unsupported or invalid private key encoding\n",stderr);
done:
    ssh_wipe(data,size);free(data);if(decoded){ssh_wipe(decoded,size);free(decoded);}ssh_wipe(password,sizeof(password));
    if(r){ssh_wipe(k,sizeof(*k));errno=EINVAL;return -1;}return 0;
}
