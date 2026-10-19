unsigned short cells[64010];
struct Pair {int a; long b;};
struct Pair seed={.b=70000L,.a=3};
volatile int observed;
int twice(int n) {return n*2;}
long *choose(int flag,long *a,long *b) {return flag?a:b;}
int main(void) {
    int array[4]={1,2,3,4};
    struct Pair p={.b=11,.a=7};
    int (*callback)(int)=twice;
    unsigned short *cross=cells+(63999UL-((unsigned long)cells&65535UL));
    cross[0]=10;cross[1]=20;cross[2]=30;
    if((cross+2)[-2]!=10)return 5;
    if(cross[1]!=20 || cross[2]!=30 || cross+2-cross!=2)return 1;
    if((unsigned long)(cross+2)-(unsigned long)cross==2)return 2;
    if(array[3]!=4 || seed.a!=3 || seed.b!=70000L || p.a!=7 || p.b!=11)return 3;
    observed=callback(p.a);
    if(observed!=14)return 4;
    if(choose(0,&seed.b,0)!=0 || choose(1,&seed.b,0)!=&seed.b)return 6;
    return 0;
}
