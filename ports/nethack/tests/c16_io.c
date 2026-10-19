#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d errno %d\n",__LINE__,errno); return 1; } } while (0)
static unsigned char values[65536UL], result[65536UL];
int main(void)
{
    FILE *file;
    unsigned long i;
    char small[4], readback[4];
    for (i = 0; i < 65536UL; ++i) values[i] = i;
    file = fopen("/root/c16.bin","w+"); CHECK(file);
    CHECK(fwrite(values,sizeof values,1,file) == 1);
    CHECK(ftell(file) == 65536L);
    CHECK(fseek(file,0,SEEK_SET) == 0);
    CHECK(fread(result,1,65536UL,file) == 65536UL);
    CHECK(!memcmp(values,result,sizeof values));
    CHECK(!feof(file)); CHECK(fread(result,1,1,file) == 0 && feof(file));
    CHECK(fseek(file,65534L,SEEK_SET) == 0);
    CHECK(fread(result,1,2,file) == 2 && result[0] == 65534U && result[1] == 65535U);
    CHECK(fclose(file) == 0);
    file = fopen("/root/small.bin","w+"); CHECK(file);
    CHECK(setvbuf(file,NULL,_IONBF,0) == 0);
    small[0] = 65535U; small[1] = 'x'; small[2] = '\n'; small[3] = 0;
    CHECK(fwrite(small,2,2,file) == 2);
    CHECK(fseek(file,0,SEEK_SET) == 0);
    CHECK(fread(readback,2,2,file) == 2 && !memcmp(small,readback,4));
    CHECK(fseek(file,0,SEEK_SET) == 0);
    CHECK(fread(readback,3,2,file) == 1 && feof(file));
    CHECK(fseek(file,1,SEEK_SET) == 0);
    CHECK(ungetc('z',file) == 'z');
    CHECK(fread(readback,1,2,file) == 2 && readback[0] == 'z' && readback[1] == 'x');
    CHECK(fseek(file,0,SEEK_END) == 0);
    CHECK(setvbuf(file,NULL,_IOLBF,0) == 0);
    CHECK(fwrite(small,1,4,file) == 4);
    CHECK(fclose(file) == 0);
    puts("C16 binary stdio PASS");
    return 0;
}
