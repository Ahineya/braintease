struct packed {
    unsigned long kind:3;
    unsigned long value:29;
    signed long delta:7;
    unsigned long guard:9;
};
#ifdef BITFIELD_GLOBALS
static struct packed initial = {5, 0x1234567UL, -17, 257};
#endif
static struct packed *once(struct packed *p, unsigned *visits) { (*visits)++; return p; }
int main(void)
{
#ifndef BITFIELD_GLOBALS
    struct packed initial = {5, 0x1234567UL, -17, 257};
#endif
    struct packed v = {3, 12345, -3, 511};
    unsigned visits = 0;
    unsigned long old;
    if (initial.kind != 5 || initial.value != 0x1234567UL || initial.delta != -17 || initial.guard != 257) return 1;
    if (v.kind != 3 || v.value != 12345 || v.delta != -3 || v.guard != 511) return 2;
    v.kind = 7;
    v.value += 1;
    if (v.kind != 7 || v.value != 12346 || v.guard != 511) return 3;
    if ((v.delta = 63) != 63 || v.delta != 63) return 4;
    old = once(&v, &visits)->value++;
    if (visits != 1 || old != 12346 || v.value != 12347 || v.kind != 7) return 5;
    if (++v.delta != -64 || v.delta != -64 || v.guard != 511) return 6;
    if ((v.kind = 10) != 2 || v.kind != 2) return 7;
    initial.value = v.value;
    if (initial.kind != 5 || initial.value != 12347 || initial.delta != -17 || initial.guard != 257) return 8;
    return 0;
}
