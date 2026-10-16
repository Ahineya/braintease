/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "user.h"
static int value = 7;
int main(void) {
    long p;
    int status;
    out("forkcheck: starting\n");
    p = call(32, 0, 0, 0, 0);
    if (p < 0) {
        out("fork failed\n");
        return 1;
    }
    if (!p) {
        value = 99;
        out("child value=");
        number(value);
        out("\n");
        call(0, 23, 0, 0, 0);
        for (;;)
            ;
    }
    call(55, p, (long)&status, 0, 0);
    out("parent value=");
    number(value);
    out(" child exit=");
    number((unsigned)status >> 8);
    out("\n");
    return value == 7 && ((unsigned)status >> 8) == 23 ? 0 : 1;
}
