#ifndef ROS_H
#define ROS_H

#define INT21_GETCHAR 0x01
#define INT21_PUTCHAR 0x02
#define INT21_PUTS    0x09
#define INT21_EXIT    0x4C

unsigned ros_int21(unsigned ah, unsigned a1, unsigned a2, unsigned a3);

#endif
