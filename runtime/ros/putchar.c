#include <ros.h>

void putchar(int c) {
    ros_int21(INT21_PUTCHAR, (unsigned)c, 0, 0);
}
