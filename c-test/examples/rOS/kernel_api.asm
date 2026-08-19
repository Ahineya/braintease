; Naked KERNEL trampolines (PCB 2). Do not touch SP or RA.

fat16_mount:
    LI T0, 2
    LI T1, 0
    JALR R0, T0, T1

fat16_lookup:
    LI T0, 2
    LI T1, 1
    JALR R0, T0, T1

fat16_resolve:
    LI T0, 2
    LI T1, 2
    JALR R0, T0, T1

fat16_dir_open:
    LI T0, 2
    LI T1, 3
    JALR R0, T0, T1

fat16_dir_next:
    LI T0, 2
    LI T1, 4
    JALR R0, T0, T1

fat16_read_at:
    LI T0, 2
    LI T1, 5
    JALR R0, T0, T1

k_exec:
    LI T0, 2
    LI T1, 6
    JALR R0, T0, T1

k_exit:
    LI T0, 2
    LI T1, 7
    JALR R0, T0, T1
