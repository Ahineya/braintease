#include <stdio.h>
#include <mmio.h>
#include <mmio_constants.h>
#include "fat16.h"
#include "loader.h"
#include "imem.h"
#include "sys.h"
#include "bootmsg.h"

static Fat16Fs g_kfs;

void io_putchar(int c);
int io_getchar(void);

unsigned int21_dispatch(unsigned ah, unsigned a1, unsigned a2, unsigned a3) {
    unsigned i;
    unsigned c;
    unsigned bank;

    if (ah == INT21_GETCHAR) {
        return (unsigned)io_getchar();
    }
    if (ah == INT21_PUTCHAR) {
        io_putchar((int)a1);
        return 0;
    }
    if (ah == INT21_PUTS) {
        bank = a2;
        if (a2 == (unsigned)-1) {
            bank = ROS_GP_APP;
        }
        i = a1;
        while (1) {
            c = dmem_load(bank, i);
            if (c == 0) {
                break;
            }
            io_putchar((int)c);
            i = i + 1;
        }
        return 0;
    }
    if (ah == INT21_EXIT) {
        ros_set_status(a1);
        irq_ack();
        ros_restore();
        return 0;
    }
    (void)a3;
    return (unsigned)-1;
}

int k_exit(int status) {
    ros_set_status((unsigned)status);
    return 0;
}

int k_exec(char *path) {
    Fat16Fs fs;
    Fat16DirEnt ent;
    unsigned char mag[4];
    int r;

    r = fat16_mount(&fs);
    if (r != FAT16_OK) {
        return r;
    }
    r = fat16_resolve(&fs, 0, path, &ent);
    if (r != FAT16_OK) {
        return r;
    }
    r = fat16_read_at(&fs, ent.cluster, ent.size, 0, mag, 4);
    if (r != 4) {
        return FAT16_ERR;
    }
    if (mag[0] == RXE1_MAGIC0 && mag[1] == RXE1_MAGIC1 &&
        mag[2] == RXE1_MAGIC2 && mag[3] == RXE1_MAGIC3) {
        return rxe_load_and_enter(&fs, &ent, ROS_IMEM_APP, ROS_GP_APP, ROS_SB_APP);
    }
    return FAT16_ERR;
}

int main(void) {
    Fat16DirEnt ent;
    int r;

    boot_stage("KERNEL.SYS");

    irq_set_vector(ROS_IMEM_KERNEL, K_SLOT_INT21);
    irq_set_enable(IRQ_SW);
    boot_item("INT21", "vector 8");

    r = fat16_mount(&g_kfs);
    if (r != FAT16_OK) {
        boot_fail("no FAT16 volume");
        return 1;
    }
    r = fat16_lookup(&g_kfs, 0, "COMMAND.COM", &ent);
    if (r != FAT16_OK) {
        boot_fail("COMMAND.COM not found");
        return 1;
    }
    boot_load("COMMAND.COM");
    sys1_load_and_enter(&g_kfs, &ent);
    boot_fail("COMMAND returned");
    return 1;
}
