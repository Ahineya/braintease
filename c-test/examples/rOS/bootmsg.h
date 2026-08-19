#ifndef ROS_BOOTMSG_H
#define ROS_BOOTMSG_H

#define BOOT_RESET 0
#define BOOT_BOLD  1
#define BOOT_RED   31
#define BOOT_GREEN 32
#define BOOT_YEL   33
#define BOOT_BLUE  34
#define BOOT_MAG   35
#define BOOT_CYAN  36
#define BOOT_WHITE 37
#define BOOT_DIM   90

void boot_sgr(int n);
void boot_stage(char *name);
void boot_item(char *label, char *value);
void boot_load(char *name);
void boot_fail(char *msg);
void boot_banner(char *title);
void boot_palette(void);

#endif
