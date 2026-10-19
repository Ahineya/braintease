#include <stdio.h>
#include <stdlib.h>
#ifndef FLOAT_STANDALONE
#include <math.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#else
#include <vm_devices.h>
#include <__float_accel.h>
#endif
#include <vm_math.h>
static unsigned checks, failures;
#define C(x) do { checks++; if (!(x)) { printf("FAIL %u\n", (unsigned)__LINE__); failures++; } } while (0)
extern unsigned long long reference_f64_add(unsigned long long, unsigned long long);
extern unsigned long long reference_f64_mul(unsigned long long, unsigned long long);
extern unsigned long long reference_f64_div(unsigned long long, unsigned long long);
extern unsigned long reference_f32_add(unsigned long, unsigned long);
extern unsigned long reference_f32_mul(unsigned long, unsigned long);
extern unsigned long reference_f32_div(unsigned long, unsigned long);
#ifndef FLOAT_STANDALONE
static int near(double a, double b) { return fabs(a-b) < 1e-13; }
#endif
static unsigned long millis(void) {
#ifdef FLOAT_STANDALONE
    VmHostTime t;
    if(vm_host_monotonic(&t)) { failures++; return 0; }
    return t.seconds_low*1000UL+t.nanoseconds/1000000UL;
#else
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) { failures++; return 0; }
    return t.tv_sec*1000UL + t.tv_nsec/1000000UL;
#endif
}
static void arithmetic(void) {
    volatile double a = 1.25, b = 1.5;
    volatile float x = 1.25f, y = 1.5f;
    union { double f; unsigned long long u; } value;
    C(a+b == 2.75); C(a-b == -0.25); C(a*b == 1.875); C(a/b == 5.0/6.0);
    C(x+y == 2.75f); C(x-y == -0.25f); C(x*y == 1.875f); C(x/y == 5.0f/6.0f);
    value.u = 3; a = value.f; b = 0.5; value.f = a*b; C(value.u == 2);
    a = -0.0; value.f = a*b; C(value.u == 0x8000000000000000ULL);
}
#ifndef FLOAT_STANDALONE
extern long call(unsigned, long, long, long, long);
static void kernel_probe(void) {
    VmMathRequest packet = {1,16,0,0,0,0,0x1234};
    C(call(27,(long)&packet,RIPPLE_TIME_MATH,0,0)==0 && packet.result==7);
    packet.operation=0xffff;packet.result=0x1234;
    C(call(27,(long)&packet,RIPPLE_TIME_MATH,0,0)==VM_DEVICE_UNSUPPORTED && packet.result==0x1234);
    packet.length=15;
    C(call(27,(long)&packet,RIPPLE_TIME_MATH,0,0)==VM_DEVICE_BAD_DESCRIPTOR && packet.result==0x1234);
    packet.length=16;packet.version=2;
    C(call(27,(long)&packet,RIPPLE_TIME_MATH,0,0)==VM_DEVICE_BAD_VERSION && packet.result==0x1234);
}
static void math(void) {
    /* Actual function pointers exercise the ABI QuickJS's built-in table uses. */
    double (*unary[])(double) = {sqrt,floor,ceil,trunc,round,sin,cos,tan,asin,acos,atan,
        sinh,cosh,tanh,asinh,acosh,atanh,exp,expm1,log,log1p,log2,log10,cbrt,fabs};
    double input[] = {4,-1.2,-1.2,-1.2,-1.5,0,0,0,0,1,0,0,0,0,0,1,0,0,0,1,0,8,100,27,-2};
    double expected[] = {2,-2,-1,-1,-2,0,1,0,0,0,0,0,1,0,0,0,0,1,0,0,0,3,2,3,2};
    unsigned i;
    for (i=0; i<sizeof(input)/sizeof(input[0]); i++) C(near(unary[i](input[i]),expected[i]));
    C(near(atan2(1.,1.),M_PI/4)); C(pow(2.,10.)==1024.); C(hypot(3.,4.)==5.);
    C(fmod(5.5,2.)==1.5); C(copysign(2.,-1.)==-2.);
    C(rint(2.5)==2.); C(rint(3.5)==4.); C(lrint(-2.5)==-2L);
    C(lround(-2.5)==-3L); C(nearbyint(-3.5)==-4.);
    C(isnan(acosh(0.))); C(isnan(sqrt(-1.))); C(isinf(log(0.)) && signbit(log(0.)));
    C(signbit(sin(-0.))); C(signbit(fmin(0.,-0.))); C(!signbit(fmax(-0.,0.)));
    C(fmin(sqrt(-1.),3.)==3.); C(fmax(3.,sqrt(-1.))==3.);
    C(near(log1p(1e-15),1e-15)); C(near(expm1(1e-15),1e-15));
}
#endif
#ifdef FLOAT_STANDALONE
#define MATH_BATCH 5000
#else
#define MATH_BATCH 500
#endif
static void bench(void) {
    unsigned i, round; unsigned long start, sample, fast, slow;
    volatile unsigned long long answer64;
    volatile unsigned long answer32;
    volatile double a=1.234567890123, b=1.345678901234;
    volatile float x=1.2345f, y=1.3456f;
    union { double f; unsigned long long u; } da, db, dr;
    union { float f; unsigned long u; } fa, fb, fr;
    da.f=a; db.f=b; fa.f=x; fb.f=y;
#define B64(name, op) do { \
    fast=slow=0xffffffffUL; \
    for(round=0;round<5;round++) { \
    start=millis(); for(i=0;i<MATH_BATCH;i++) { dr.f=a op b; answer64=dr.u; } sample=millis()-start; \
    if(round && sample<fast) fast=sample; \
    start=millis(); for(i=0;i<MATH_BATCH;i++) answer64=reference_f64_##name(da.u,db.u); sample=millis()-start; \
    if(round && sample<slow) slow=sample; } \
    C(answer64==dr.u); printf("MATH_BENCH f64_" #name " %lu %lu\n",fast,slow); \
} while(0)
#define B32(name, op) do { \
    fast=slow=0xffffffffUL; \
    for(round=0;round<5;round++) { \
    start=millis(); for(i=0;i<MATH_BATCH;i++) { fr.f=x op y; answer32=fr.u; } sample=millis()-start; \
    if(round && sample<fast) fast=sample; \
    start=millis(); for(i=0;i<MATH_BATCH;i++) answer32=reference_f32_##name(fa.u,fb.u); sample=millis()-start; \
    if(round && sample<slow) slow=sample; } \
    C(answer32==fr.u); printf("MATH_BENCH f32_" #name " %lu %lu\n",fast,slow); \
} while(0)
    B64(add,+); B64(mul,*); B64(div,/);
    B32(add,+); B32(mul,*); B32(div,/);
}
int main(void) {
    unsigned long long result = 0x1234;
#ifndef FLOAT_STANDALONE
    unsigned i; int status; pid_t child;
#endif
    C(sizeof(VmMathRequest)==VM_MATH_CELLS);
#ifndef FLOAT_STANDALONE
    errno=123;
#endif
    C(__rcc_float_accel(VM_MATH_QUERY,0,0,&result)==0 && result==7);
    arithmetic();
#ifndef FLOAT_STANDALONE
    C(errno==123);
    math(); kernel_probe();
    child=fork(); C(child>=0);
    if (!child) {
        for (i=0;i<200;i++) if (pow(2.,10.)!=1024. || sin(0.)!=0.) _exit(1);
        _exit(0);
    }
    for (i=0;i<200;i++) C(hypot(3.,4.)==5.);
    C(waitpid(child,&status,0)==child && status==0);
    #endif
    bench();
    printf("FLOAT_ACCEL checks=%u failures=%u\n",checks,failures);
    return failures?1:0;
}
