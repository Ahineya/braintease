#include <stdio.h>
#include <unistd.h>
int main(int argc, char **argv) {
    int i;
    printf("Independent executable, PID %d",getpid());
    for(i=1;i<argc;i++) printf(" %s",argv[i]);
    putchar('\n');
    return 0;
}
