#include <stdio.h>

typedef volatile unsigned short Register;
typedef unsigned short RegisterArray[2];
struct Device { unsigned short value; unsigned short words[2]; };

static Register global;
static volatile RegisterArray values;
static volatile struct Device device;

static int fail(int number)
{
    printf("volatile failure %d\n", number);
    return 1;
}

static unsigned short read_target(unsigned short choose, unsigned short *ordinary,
                                  volatile unsigned short *special)
{
    return *(choose ? ordinary : special);
}

int main(void)
{
    volatile unsigned short local = 3;
    unsigned short ordinary = 9;
    unsigned short *volatile pointer = &ordinary;
    volatile unsigned short *rng = (volatile unsigned short *)4UL;
    volatile unsigned short *seed = (volatile unsigned short *)5UL;
    struct Device copy;
    unsigned short before;
    global = local++;
    if (global != 3 || local != 4) return fail(1);
    global += 7;
    if (global != 10 || ++global != 11) return fail(2);
    values[0] = global;
    values[1] = local;
    if (values[0] != 11 || values[1] != 4) return fail(3);
    if (!pointer || *pointer != 9 || read_target(0, &ordinary, &global) != 11) return fail(4);
    device.value = 42;
    device.words[0] = 17;
    device.words[1] = 29;
    copy = device;
    if (copy.value != 42 || copy.words[0] != 17 || copy.words[1] != 29) return fail(5);
    copy.value = 63;
    device = copy;
    if (device.value != 63) return fail(6);

    /* RNG reads are observable even when their result is discarded. Its low
     * state follows the same 16-bit LCG independently of the initial high half. */
    *seed = 1;
    *rng;
    (void)*rng;
    if (*seed != 34267U) return fail(7);
    before = *seed;
    (void)sizeof(*rng);
    (void)&*rng;
    if (*seed != before) return fail(8);
    (*rng)++;
    if (*seed != 382U) return fail(9);
    *rng += 1;
    if (*seed != 15045U) return fail(10);
    puts("volatile semantics ok");
    return 0;
}
