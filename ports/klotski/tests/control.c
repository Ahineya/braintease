/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Independent Nano-X client exercises application resize notifications. */
#include <stdio.h>
#include <stdlib.h>
#include "nano-X.h"
int main(int argc,char **argv)
{
    GR_WINDOW_ID w;
    if(argc!=3||GrOpen()<0)return 1;
    w=GrGetFocus();if(!w||w==GR_ROOT_WINDOW_ID){GrClose();return 1;}
    GrResizeWindow(w,atoi(argv[1]),atoi(argv[2]));GrFlush();GrClose();
    puts("KLOTSKI_CONTROL_OK");return 0;
}
