/* Development staging: reuse pkg's bounded downloader and object verification.
 * Pass the expected digest through an authenticated control connection. Only
 * a verified temporary file replaces the explicit destination. */
#include "../../../ports/pkg/pkg.h"
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
int main(int argc,char **argv){
    char temporary[512],hash[65];unsigned long size;
    if(argc!=4||!pkg_digest_ok(argv[3])||strlen(argv[2])>480){
        puts("Usage: stage-fetch HTTP_URL DESTINATION EXPECTED_SHA256");return 2;
    }
    snprintf(temporary,sizeof(temporary),"%s.part",argv[2]);
    if(access(temporary,F_OK)==0){fputs("stage-fetch: temporary file already exists\n",stderr);return 1;}
    printf("Downloading %s\n",argv[2]);fflush(stdout);
    if(pkg_download(argv[1],temporary,PKG_LIMIT)||pkg_hash_file(temporary,hash,&size)){
        fprintf(stderr,"stage-fetch: %s\n",pkg_error);unlink(temporary);return 1;
    }
    if(strcmp(hash,argv[3])){fputs("stage-fetch: SHA-256 mismatch\n",stderr);unlink(temporary);return 1;}
    if(chmod(temporary,0755)||rename(temporary,argv[2])){perror("stage-fetch publish");unlink(temporary);return 1;}
    printf("Verified %s %lu %s\n",hash,size,argv[2]);return 0;
}
