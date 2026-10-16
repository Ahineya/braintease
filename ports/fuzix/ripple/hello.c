/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "user.h"
int main(int argc, char **argv, char **envp) {
    (void)envp;
    out("Hello from a separate RCC userspace executable! pid=");
    number((unsigned)call(18, 0, 0, 0, 0));
    out("\n");
    if (argc > 1) {
        out("argument: ");
        out(argv[1]);
        out("\n");
    }
    return 0;
}
