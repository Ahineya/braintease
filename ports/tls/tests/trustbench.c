/* Diagnostic only: compare PEM decoding paths on public trust anchors. */
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/coding.h>
#include <wolfssl/wolfcrypt/sha.h>
#include <wolfssl/wolfcrypt/hash.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static unsigned long stamp(void) {
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t))exit(1);
    return (unsigned long)t.tv_sec*1000UL+t.tv_nsec/1000000UL;
}
int main(int argc,char **argv) {
    struct stat st;
    WOLFSSL_CTX *ctx;
    char *data,*at,*end,*header;
    char saved;
    byte *ct,*table,hash[WC_SHA_DIGEST_SIZE];
    word32 n,m;
    unsigned long start,read_ms,ct_ms=0,table_ms=0,sha_ms=0,count=0,used=0;
    ssize_t got;
    int fd;
    if(argc!=2)return 2;
    start=stamp();
    fd=open(argv[1],O_RDONLY);
    if(fd<0||fstat(fd,&st)||st.st_size<=0||st.st_size>512000L)return 1;
    data=malloc(st.st_size+1);ct=malloc(8192);table=malloc(8192);
    if(!data||!ct||!table)return 1;
    while(used<(unsigned long)st.st_size) {
        got=read(fd,data+used,(unsigned long)st.st_size-used>4096?4096:(unsigned long)st.st_size-used);
        if(got<=0)return 1;
        used+=got;
    }
    close(fd);data[used]=0;read_ms=stamp()-start;
    at=data;
    while((at=strstr(at,"-----BEGIN CERTIFICATE-----"))!=NULL) {
        at+=27;
        end=strstr(at,"-----END CERTIFICATE-----");
        if(!end||end-at>8192)return 1;
        n=m=8192;
        start=stamp();
        if(Base64_Decode((byte *)at,end-at,ct,&n))return 1;
        ct_ms+=stamp()-start;
        start=stamp();
        if(Base64_Decode_nonCT((byte *)at,end-at,table,&m))return 1;
        table_ms+=stamp()-start;
        if(n!=m||memcmp(ct,table,n))return 1;
        start=stamp();
        if(wc_ShaHash(ct,n,hash))return 1;
        sha_ms+=stamp()-start;
        ++count;at=end+25;
    }
    if(wolfSSL_Init()!=WOLFSSL_SUCCESS)return 1;
    ctx=wolfSSL_CTX_new(wolfTLS_client_method());
    if(!ctx)return 1;
    wolfSSL_CTX_set_verify(ctx,WOLFSSL_VERIFY_PEER,NULL);
    start=stamp();
    if(wolfSSL_CTX_load_verify_buffer(ctx,(byte *)data,used,WOLFSSL_FILETYPE_PEM)!=WOLFSSL_SUCCESS)return 1;
    printf("TRUST_BENCH: %lu roots, read %lu ms, base64 CT %lu ms, base64 table %lu ms, SHA1 of DER %lu ms, load %lu ms\n",count,read_ms,ct_ms,table_ms,sha_ms,stamp()-start);
    /* Both malformed ASCII and wide C16 cells must be rejected before table
       indexing. Use just one PEM block so other valid roots cannot mask it. */
    header=strstr(data,"-----BEGIN CERTIFICATE-----");
    if(!header)return 1;
    at=header+27;
    while(*at=='\r'||*at=='\n'||*at==' '||*at=='\t')++at;
    end=strstr(at,"-----END CERTIFICATE-----");
    if(!end)return 1;
    saved=*at;*at='!';
    if(wolfSSL_CTX_load_verify_buffer(ctx,(byte *)header,end+25-header,WOLFSSL_FILETYPE_PEM)==WOLFSSL_SUCCESS)return 1;
    *at=0x100;
    if(wolfSSL_CTX_load_verify_buffer(ctx,(byte *)header,end+25-header,WOLFSSL_FILETYPE_PEM)==WOLFSSL_SUCCESS)return 1;
    *at=saved;
    puts("TRUST_INPUT_REJECTION_OK");
    wolfSSL_CTX_free(ctx);free(data);free(ct);free(table);
    return 0;
}
