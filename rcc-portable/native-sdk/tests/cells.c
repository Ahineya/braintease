#include <stddef.h>
extern void *__rcc_cell_copy(void *,const void *,size_t);
extern void *__rcc_cell_fill(void *,int,size_t);
int main(void)
{
    unsigned char cells[64020];
    unsigned long i;
    __rcc_cell_fill(cells,0xabcd,64020UL);
    if(cells[0]!=0xabcdU || cells[64019]!=0xabcdU) return 1;
    for(i=0;i<64020UL;i++)cells[i]=(unsigned)(i+511UL);
    if(__rcc_cell_copy(cells+1,cells,64019UL)!=cells+1) return 2;
    for(i=1;i<64020UL;i++)if(cells[i]!=(unsigned)(i+510UL))return 3;
    __rcc_cell_copy(cells,cells+1,64019UL);
    for(i=0;i<64019UL;i++)if(cells[i]!=(unsigned)(i+511UL))return 4;
    __rcc_cell_copy(cells,cells,0);
    return 0;
}
