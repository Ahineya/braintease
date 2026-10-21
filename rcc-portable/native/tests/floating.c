/* FP stays typed until QBE selects native instructions; memory/calls carry
 * exact IEEE bits, including signed zero and unordered comparison. */
static double square(double x) { return x*x+1.5; }
static float twice(float x) { return x+x; }
int main(void)
{
    volatile double value=2.5;
    volatile float small=1.25f;
    double zero=0.0, negative=-0.0;
    if (square(value)!=7.75) return 1;
    if (twice(small)!=2.5f) return 2;
    if (zero!=negative || negative<zero) return 3;
    return 0;
}
