/* C99 6.7.6.3 paragraph 7: a parameter declared as "array of T" is
 * adjusted to "pointer to T".  Stores through it update the caller's
 * object.  Reads see the caller's elements.
 */
#include <stdio.h>

void fill4(char out[4]) {
    out[0] = 'A';
    out[1] = 'B';
    out[2] = 'C';
    out[3] = 0;
}

int sum3(int xs[3]) {
    return xs[0] + xs[1] + xs[2];
}

int main() {
    char buf[4];
    int nums[3];

    buf[0] = 'Z';
    fill4(buf);
    if (buf[0] == 'A' && buf[1] == 'B' && buf[2] == 'C') {
        putchar('Y');
    } else {
        putchar('N');
    }

    nums[0] = 1;
    nums[1] = 2;
    nums[2] = 3;
    if (sum3(nums) == 6) {
        putchar('Y');
    } else {
        putchar('N');
    }

    putchar('\n');
    return 0;
}
