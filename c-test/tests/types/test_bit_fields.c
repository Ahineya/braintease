void putchar(int);
union Packed {
    unsigned int word;
    struct { unsigned int a:3, :2, b:4; signed int c:7; } bits;
};
struct Split { unsigned a:12; unsigned b:8; unsigned :0; unsigned c:4; int tail; };
struct Mixed { int head; unsigned a:3, b:5; long tail; };
struct Mixed global = { 71, 5, 19, 123456L };
struct Split split = { 11, 22, 3, 91 };
struct Wide { unsigned long a:20, b:12; signed long c:19; };
struct Wide wide = { 0xabcdeUL, 0x987UL, -12345L };
struct Narrow { unsigned long n:3; signed long long s:4; _Bool b:1; };
struct Narrow narrow = { 6, -3, 2 };
union Direct { unsigned a:3; signed b:4; };
union Direct direct = { .b = -2 };
struct Full { unsigned long long a:64; };
struct Full full = { 0xfedcba9876543210ULL };
typedef int (callback)(int);
static int calls;
union Packed *once(union Packed *p) { calls++; return p; }
int main(void) {
    union Packed p;
    struct Mixed m = { 7, 2, 9, 76543L };
    struct Mixed copy;
    volatile union Packed v;
    p.word = 0;
    p.bits.a = 5;
    p.bits.b = 9;
    p.bits.c = -3;
    putchar(p.word == (5U | (9U<<5) | (125U<<9)) ? 'Y' : 'N');
    putchar(p.bits.c == -3 && p.bits.b == 9 ? 'Y' : 'N');
    p.bits.a = 7;
    putchar(p.bits.a++ == 7 && p.bits.a == 0 && p.bits.b == 9 ? 'Y' : 'N');
    putchar(++p.bits.a == 1 && (p.bits.a = 14) == 6 ? 'Y' : 'N');
    calls = 0;
    once(&p)->bits.b += 15;
    putchar(calls == 1 && p.bits.b == 8 && p.bits.a == 6 ? 'Y' : 'N');
    putchar(sizeof p == 1 && sizeof(struct Split) == 4 && split.a == 11 && split.b == 22 && split.c == 3 && split.tail == 91 ? 'Y' : 'N');
    copy = m;
    putchar(sizeof m == 4 && global.head == 71 && global.a == 5 && global.b == 19 && global.tail == 123456L && copy.b == 9 && copy.tail == 76543L ? 'Y' : 'N');
    putchar(sizeof wide == 4 && wide.a == 0xabcdeUL && wide.b == 0x987UL && wide.c == -12345L ? 'Y' : 'N');
    wide.a += 5;
    wide.c--;
    putchar(wide.a == 0xabce3UL && wide.b == 0x987UL && wide.c == -12346L ? 'Y' : 'N');
    v.word = 0xffff;
    v.bits.b &= 2;
    putchar(v.bits.b == 2 && v.bits.a == 7 && v.bits.c == -1 ? 'Y' : 'N');
    putchar(p.bits.a - 7 < 0 ? 'Y' : 'N');
    {
        struct Narrow n = { 7, -2, 2 };
        union Direct d = { .b = -3 };
        int (array)[3] = { 4, 5, 6 };
        callback *cb;
        n.b = &m;
        ++n.b;
        putchar(narrow.n == 6 && narrow.s == -3 && narrow.b == 1 && n.b == 1 ? 'Y' : 'N');
        putchar(n.n++ == 7 && n.n == 0 && --n.s == -3 ? 'Y' : 'N');
        n.n += 6;
        putchar(n.n == 6 && n.n - 7 < 0 && n.s + 4 == 1 ? 'Y' : 'N');
        putchar(d.b == -3 && direct.b == -2 ? 'Y' : 'N');
        putchar(full.a == 0xfedcba9876543210ULL && sizeof full == 4 && array[2] == 6 ? 'Y' : 'N');
    }
    putchar('\n');
    return 0;
}
