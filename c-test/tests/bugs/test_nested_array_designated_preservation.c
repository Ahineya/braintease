#include <stdio.h>

static const char *names[2][3] = {
    [0][0] = "a", [0][1] = "b", [0][2] = "c",
    [1][0] = "d", [1][1] = "e", [1][2] = "f"
};

int main(void)
{
    putchar(names[0][0] && names[0][1] && names[0][2] &&
            names[1][0] && names[1][1] && names[1][2] &&
            names[0][0][0] == 'a' && names[1][1][0] == 'e' ? 'Y' : 'N');
    return 0;
}
