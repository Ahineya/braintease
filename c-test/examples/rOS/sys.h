#ifndef ROS_SYS_H
#define ROS_SYS_H

#include <stdint.h>

#define SYS1_MAGIC0 'S'
#define SYS1_MAGIC1 'Y'
#define SYS1_MAGIC2 'S'
#define SYS1_MAGIC3 '1'

#define SYS1_HEADER_SIZE 24

#define ROS_IMEM_BIOS 0
#define ROS_IMEM_IO 1
#define ROS_IMEM_KERNEL 2
#define ROS_IMEM_COMMAND 3
#define ROS_IMEM_APP 32

#define ROS_GP_BIOS 1
#define ROS_SB_KERNEL 2
#define ROS_GP_IO 3
#define ROS_GP_KERNEL 4
#define ROS_GP_COMMAND 6
#define ROS_SB_COMMAND 7

#define IO_SLOT_PUTCHAR 0
#define IO_SLOT_GETCHAR 1
#define IO_SLOT_DISK_READ8 2
#define IO_SLOT_DISK_WRITE8 3
#define IO_SLOT_DISK_READ16 4
#define IO_SLOT_DISK_READ32 5
#define IO_SLOT_DISK_READ 6

#define K_SLOT_MOUNT 0
#define K_SLOT_LOOKUP 1
#define K_SLOT_RESOLVE 2
#define K_SLOT_DIR_OPEN 3
#define K_SLOT_DIR_NEXT 4
#define K_SLOT_READ_AT 5
#define K_SLOT_EXEC 6
#define K_SLOT_EXIT 7

#define INT21_GETCHAR 0x01
#define INT21_PUTCHAR 0x02
#define INT21_PUTS 0x09
#define INT21_OPEN 0x3D
#define INT21_READ 0x3F
#define INT21_WRITE 0x40
#define INT21_CLOSE 0x3E
#define INT21_EXEC 0x4B
#define INT21_EXIT 0x4C

typedef struct Sys1Header {
    uint16_t bank_size;
    uint16_t code_bank;
    uint16_t gp_bank;
    uint16_t sb_bank;
    uint32_t entry;
    uint32_t insn_count;
    uint32_t data_size;
} Sys1Header;

#endif
