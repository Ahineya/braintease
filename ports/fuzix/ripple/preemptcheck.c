/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "user.h"
static volatile unsigned long counter;
int main(void) {
    long p;
    int status;
    p = call(32, 0, 0, 0, 0);
    if (p < 0)
        return 1;
    if (!p) {
        out("child ran while parent computed\n");
        call(0, 0, 0, 0, 0);
        for (;;)
            ;
    }
    for (counter = 0; counter < 200000UL; counter++)
        ;
    out("parent computation finished\n");
    call(55, p, (long)&status, 0, 0);
    out("preemptcheck complete\n");
    return 0;
}
