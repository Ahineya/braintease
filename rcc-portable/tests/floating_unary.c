#include <stdbool.h>
volatile double dzero = 0.0, dvalue = 1.5;
volatile float fzero = 0.0f, fvalue = 1.5f;
union Double {double value; unsigned long long bits;};
union Float {float value; unsigned long bits;};
double negative_double(double n) {return -n;}
float negative_float(float n) {return -n;}
bool truth_double(double n) {return n;}
bool truth_float(float n) {return n;}
int main(void) {
    union Double d; union Float f;
    if (truth_double(dzero) || truth_float(fzero)) return 1;
    if (!truth_double(dvalue) || !truth_float(fvalue)) return 2;
    if (negative_double(dvalue)!=-1.5 || negative_float(fvalue)!=-1.5f) return 3;
    d.value=negative_double(dzero);f.value=negative_float(fzero);
    if (d.bits!=0x8000000000000000ULL || f.bits!=0x80000000UL) return 4;
    if (truth_double(d.value) || truth_float(f.value)) return 5;
    d.value=negative_double(d.value);f.value=negative_float(f.value);
    if (d.bits || f.bits) return 6;
    d.bits=0x7ff8000000000042ULL;f.bits=0x7fc00042UL;
    if (!truth_double(d.value) || !truth_float(f.value)) return 7;
    d.value=negative_double(d.value);f.value=negative_float(f.value);
    if (d.bits!=0xfff8000000000042ULL || f.bits!=0xffc00042UL) return 8;
    return 0;
}
