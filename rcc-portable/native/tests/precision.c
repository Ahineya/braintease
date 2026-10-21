static float narrow(double x) { return (float)x; }
static double widen(float x) { return (double)x; }
int main(void)
{
    union { float f; unsigned long bits; } single;
    union { double f; unsigned long long bits; } wide;
    single.f=narrow(1.5);
    if(single.bits!=0x3fc00000UL) return 1;
    wide.bits=0x8000000000000000ULL;
    single.f=narrow(wide.f);
    if(single.bits!=0x80000000UL) return 2;
    wide.bits=0x7ff0000000000000ULL;
    single.f=narrow(wide.f);
    if(single.bits!=0x7f800000UL) return 3;
    wide.bits=0x7ff800123456789aULL;
    single.f=narrow(wide.f);
    if((single.bits&0x7f800000UL)!=0x7f800000UL || !(single.bits&0x7fffffUL)) return 4;
    wide.bits=1ULL;
    single.f=narrow(wide.f);
    if(single.bits) return 5;
    single.bits=1UL;
    wide.f=widen(single.f);
    if(wide.bits!=0x36a0000000000000ULL) return 6;
    single.bits=0x80000000UL;
    wide.f=widen(single.f);
    return wide.bits!=0x8000000000000000ULL;
}
