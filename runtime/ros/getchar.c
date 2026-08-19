#include <ros.h>

int getchar(void) {
    return (int)ros_int21(INT21_GETCHAR, 0, 0, 0);
}
