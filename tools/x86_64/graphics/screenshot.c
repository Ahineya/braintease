/* Save visible HP pixels as P6 PPM. One RGB octet occupies one C16 char cell;
 * SFTP exports each cell's low octet, producing an ordinary byte PPM on host.
 * Capture is row-wise: moving content may change between rows. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#ifdef SCREENSHOT_HOST_TEST
extern unsigned long riplix_screen(unsigned operation,unsigned row,char *data,unsigned long capacity);
#else
/* Private native development syscall; no shared C16 kernel ABI change. */
extern long call(unsigned,long,long,long,long);
static unsigned long riplix_screen(unsigned operation,unsigned row,char *data,unsigned long capacity) {
    return call(0x7847U,operation,row,(long)data,capacity);
}
#endif
int main(int argc,char **argv) {
    const char *path="/tmp/screen.ppm";
    unsigned scale=1,argument=1;
    if(argc>1 && !strcmp(argv[1],"--scale")) {
        if(argc<3)goto usage;
        const char *number=argv[2];scale=0;
        if(!*number)goto usage;
        while(*number) {
            if(*number<'0' || *number>'9')goto usage;
            scale=scale*10+(*number++-'0');
            if(scale>16)goto usage;
        }
        if(!scale)goto usage;
        argument=3;
    }
    if(argc>(int)argument+1)goto usage;
    if(argc>(int)argument)path=argv[argument];
    unsigned long width=riplix_screen(0,0,0,0),height=riplix_screen(1,0,0,0);
    if(!width || !height || width>8192UL || height>8192UL){fputs("screenshot: display unavailable\n",stderr);return 1;}
    unsigned long samples=width*3UL;
    unsigned long output_width=(width+scale-1)/scale,output_height=(height+scale-1)/scale;
    unsigned long output_samples=output_width*3UL;
    char *row=malloc(samples);
    if(!row){fputs("screenshot: out of memory\n",stderr);return 1;}
    FILE *file=fopen(path,"wb");
    if(!file){perror(path);free(row);return 1;}
    int failed=fprintf(file,"P6\n%lu %lu\n255\n",output_width,output_height)<0;
    for(unsigned y=0;y<height && !failed;y+=scale) {
        if(riplix_screen(2,y,row,samples)!=samples){fputs("screenshot: capture failed\n",stderr);failed=1;break;}
        /* Nearest sample at x*scale/y*scale; ceil dimensions retain the last
         * partial cell. Forward compaction never overwrites an unread sample. */
        if(scale>1)for(unsigned long x=0;x<output_width;x++)
            for(unsigned c=0;c<3;c++)row[x*3+c]=row[x*scale*3+c];
        /* fwrite sizes are logical C cells; the payload values are 0..255. */
        if(fwrite(row,1,output_samples,file)!=output_samples)failed=1;
    }
    if(fclose(file)<0)failed=1;
    free(row);
    if(failed){remove(path);fputs("screenshot: output incomplete\n",stderr);return 1;}
    printf("Screenshot saved: %s (%lux%lu)\n",path,output_width,output_height);return 0;
usage:
    fputs("usage: screenshot [--scale 1..16] [output.ppm]\n",stderr);return 2;
}
