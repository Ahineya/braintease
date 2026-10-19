_Static_assert(sizeof(char)==1 && sizeof(short)==1 && sizeof(int)==1,"C16 narrow types");
_Static_assert(sizeof(long)==2 && sizeof(long long)==4 && sizeof(void*)==2,"C16 wide types");
_Static_assert(sizeof(float)==2 && sizeof(double)==4,"C16 floating types");
_Static_assert(_Alignof(double)==1,"C16 sequential layout");
_Static_assert(_Generic((unsigned char)1 + (int)1, unsigned int:1, default:0),"unsigned char promotion");
_Static_assert(_Generic((_Bool)1 + (int)1, int:1, default:0),"boolean promotion");
_Static_assert(_Generic((unsigned long)1 + (long long)1, long long:1, default:0),"wide promotions");
_Static_assert(_Generic((unsigned long long)1 + (long long)1, unsigned long long:1, default:0),"signedness");
struct Layout {char a; long b; double c;};
enum Count {ONE=1, MANY=30000};
_Static_assert(MANY+ONE==30001,"enum constants");
_Static_assert(sizeof(struct Layout)==7,"sequential cell layout");
long widen(long x) {return x;}
long long sign(long x) {return x;}
double scale(double a,float b) {return a*b+0.5;}
int main(void) {
    char ch='\x8000';
    unsigned int u=65535U;
    long x=0x12345678L;
    unsigned long long wide=0x123456789abcdef0ULL;
    enum Count count=MANY;
    if(count+ONE!=30001)return 6;
    if(ch>=0 || u/255U!=257U || widen(x)!=0x12345678L) return 1;
    if(sign(-2147483647L-1L)!=-2147483647LL-1LL) return 2;
    if((long long)(long)0x80000000UL!=-2147483647LL-1LL) return 5;
    if((wide>>32)!=0x12345678ULL || wide%256ULL!=240ULL) return 3;
    if(scale(2.25,2.0f)!=5.0 || (int)3.75!=3) return 4;
    return 0;
}
