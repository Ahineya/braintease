/* SPDX-License-Identifier: GPL-2.0-only */
#include "user.h"
int main(int argc,char **argv) {
    if (argc!=2 || strcmp(argv[0],"argcheck") || strcmp(argv[1],"hello") || argv[2] ||
        call(18,0,0,0,0)<=1 || call(19,0,0,0,0)!=1) return 97;
    out("[child argv OK]");
    return 11;
}
