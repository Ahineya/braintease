/* A document shared by the terminal and graphical Rift text engines. */
#include <stdio.h>

int main(void) {
    const char *greeting = "Привіт, світе!";
    unsigned count = 3;
    while (count--)
        puts(greeting);
    return 0;
}
