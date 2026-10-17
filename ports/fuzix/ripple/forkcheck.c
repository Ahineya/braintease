/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "user.h"
static int value = 7;
/* Data and nested stack frames straddle the 32,768-cell bulk submission
 * boundary. Each frame stays below RCC's 32,767-cell frame limit. Native
 * character patterns catch accidental octet truncation as well as isolation. */
static unsigned data[36000];
static void pattern(unsigned *stack, unsigned *outer, unsigned salt) {
    unsigned i;
    for (i = 0; i < 36000U; i++) {
        data[i] = i ^ salt;
        if (i < 18000U) stack[i] = i + salt;
        else outer[i - 18000U] = i + salt;
    }
}
static int intact(unsigned *stack, unsigned *outer, unsigned salt) {
    unsigned i;
    for (i = 0; i < 36000U; i++)
        if (data[i] != (unsigned)(i ^ salt) || (i < 18000U ? stack[i] : outer[i - 18000U]) != (unsigned)(i + salt))
            return 0;
    return 1;
}
static int fail(void) {
    out("forkcheck FAILED memory isolation\n");
    return 1;
}
static int exercise(unsigned *outer) {
    unsigned stack[18000];
    long p;
    int status;
    out("forkcheck: starting\n");
    pattern(stack, outer, 0xa55aU);
    p = call(32, 0, 0, 0, 0);
    if (p < 0) {
        out("fork failed\n");
        return 1;
    }
    if (!p) {
        long grandchild;
        if (!intact(stack, outer, 0xa55aU)) return fail();
        pattern(stack, outer, 0x5aa5U);
        value = 99;
        grandchild = call(32, 0, 0, 0, 0);
        if (grandchild < 0) return fail();
        if (!grandchild) {
            if (!intact(stack, outer, 0x5aa5U)) return fail();
            data[0] = outer[17999] = 0;
            return 17;
        }
        if (call(55, grandchild, (long)&status, 0, 0) != grandchild ||
            ((unsigned)status >> 8) != 17 || !intact(stack, outer, 0x5aa5U)) return fail();
        out("child value=");
        number(value);
        out("\n");
        call(0, 23, 0, 0, 0);
        for (;;)
            ;
    }
    if (!intact(stack, outer, 0xa55aU)) return fail();
    if (call(55, p, (long)&status, 0, 0) != p || !intact(stack, outer, 0xa55aU)) return fail();
    out("parent value=");
    number(value);
    out(" child exit=");
    number((unsigned)status >> 8);
    out("\n");
    /* Exercise large user-stack clearing and short reads from /dev/zero. */
    {
        unsigned long done = 0;
        unsigned i;
        int fd = open("/dev/zero", 0);
        if (fd < 0) return fail();
        while (done < 18000UL) {
            long n = read(fd, stack + done, 18000UL - done);
            if (n <= 0) { close(fd); return fail(); }
            done += n;
        }
        close(fd);
        for (i = 0; i < 18000U; i++) if (stack[i]) return fail();
        fd = open("/dev/zero", 0);
        if (fd < 0) return fail();
        done = 0;
        while (done < 36000UL) {
            long n = read(fd, data + done, 36000UL - done);
            if (n <= 0) { close(fd); return fail(); }
            done += n;
        }
        close(fd);
        for (i = 0; i < 36000U; i++) if (data[i]) return fail();
    }
    out("forkcheck: data/stack isolation and user zero passed\n");
    return value == 7 && ((unsigned)status >> 8) == 23 ? 0 : 1;
}

static int stress(void) {
    unsigned outer[18000];
    return exercise(outer);
}

int main(int argc, char **argv) {
    long child;
    int status;
    if (argc > 1) {
        if (!strcmp(argv[1], "--stress")) return stress();
        out("Usage: forkcheck [--stress]\n");
        return 2;
    }
    out("forkcheck: starting\n");
    child = call(32, 0, 0, 0, 0);
    if (child < 0) return fail();
    if (!child) {
        value = 99;
        out("child value=99\n");
        return 23;
    }
    if (call(55, child, (long)&status, 0, 0) != child) return fail();
    out("parent value="); number(value);
    out(" child exit="); number((unsigned)status >> 8); out("\n");
    return value == 7 && ((unsigned)status >> 8) == 23 ? 0 : 1;
}
