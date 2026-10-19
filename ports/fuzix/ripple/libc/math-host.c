/* Host-accelerated C16 libm. errno belongs to the guest, never host libc. */
#include <math.h>
#include <errno.h>
#include <vm_math.h>
static double host_math(unsigned op, double a, double b) {
    union { double value; unsigned long long bits; } x, y, z;
    x.value = a; y.value = b;
    if (__rcc_float_accel(op, x.bits, y.bits, &z.bits) != VM_DEVICE_OK) {
        errno = ENOSYS;
        z.bits = 0x7ff8000000000000ULL;
    }
    return z.value;
}
double sqrt(double a) { return host_math(VM_MATH_SQRT, a, 0.0); }
double floor(double a) {
    union { double value; unsigned long long bits; } x;
    unsigned long long mask;
    int exp;
    x.value=a; exp=(int)((x.bits >> 52) & 2047ULL);
    if(exp>=1075) return a;
    if(exp<1023) {
        if((x.bits & 0x7fffffffffffffffULL)==0) return a;
        return (x.bits >> 63) ? -1.0 : 0.0;
    }
    mask=(1ULL << (1075-exp))-1ULL;
    if((x.bits & mask)==0) return a;
    if(x.bits >> 63) x.bits += mask+1ULL;
    x.bits &= ~mask;
    return x.value;
}
double ceil(double a) { return -floor(-a); }
double trunc(double a) {
    union { double value; unsigned long long bits; } x;
    int exp;
    x.value=a; exp=(int)((x.bits >> 52) & 2047ULL);
    if(exp>=1075) return a;
    if(exp<1023) x.bits &= 0x8000000000000000ULL;
    else x.bits &= ~((1ULL << (1075-exp))-1ULL);
    return x.value;
}
double round(double a) { return host_math(VM_MATH_ROUND, a, 0.0); }
double sin(double a) { return host_math(VM_MATH_SIN, a, 0.0); }
double cos(double a) { return host_math(VM_MATH_COS, a, 0.0); }
double tan(double a) { return host_math(VM_MATH_TAN, a, 0.0); }
double asin(double a) { return host_math(VM_MATH_ASIN, a, 0.0); }
double acos(double a) { return host_math(VM_MATH_ACOS, a, 0.0); }
double atan(double a) { return host_math(VM_MATH_ATAN, a, 0.0); }
double atan2(double a, double b) { return host_math(VM_MATH_ATAN2, a, b); }
double sinh(double a) { return host_math(VM_MATH_SINH, a, 0.0); }
double cosh(double a) { return host_math(VM_MATH_COSH, a, 0.0); }
double tanh(double a) { return host_math(VM_MATH_TANH, a, 0.0); }
double asinh(double a) { return host_math(VM_MATH_ASINH, a, 0.0); }
double acosh(double a) { return host_math(VM_MATH_ACOSH, a, 0.0); }
double atanh(double a) { return host_math(VM_MATH_ATANH, a, 0.0); }
double exp(double a) { return host_math(VM_MATH_EXP, a, 0.0); }
double expm1(double a) { return host_math(VM_MATH_EXPM1, a, 0.0); }
double log(double a) { return host_math(VM_MATH_LOG, a, 0.0); }
double log1p(double a) { return host_math(VM_MATH_LOG1P, a, 0.0); }
double log2(double a) { return host_math(VM_MATH_LOG2, a, 0.0); }
double log10(double a) { return host_math(VM_MATH_LOG10, a, 0.0); }
double pow(double a, double b) { return host_math(VM_MATH_POW, a, b); }
double cbrt(double a) { return host_math(VM_MATH_CBRT, a, 0.0); }
double hypot(double a, double b) { return host_math(VM_MATH_HYPOT, a, b); }
double fabs(double a) {
    union { double value; unsigned long long bits; } x;
    x.value=a; x.bits &= 0x7fffffffffffffffULL; return x.value;
}
double fmod(double a, double b) { return host_math(VM_MATH_FMOD, a, b); }
double copysign(double a, double b) {
    union { double value; unsigned long long bits; } x,y;
    x.value=a; y.value=b;
    x.bits=(x.bits & 0x7fffffffffffffffULL) | (y.bits & 0x8000000000000000ULL);
    return x.value;
}
double fmin(double a, double b) { return host_math(VM_MATH_FMIN, a, b); }
double fmax(double a, double b) { return host_math(VM_MATH_FMAX, a, b); }
double rint(double a) { return host_math(VM_MATH_RINT, a, 0.0); }
double nearbyint(double a) { return rint(a); }
long lrint(double a) { return (long)rint(a); }
long lround(double a) { return (long)round(a); }
