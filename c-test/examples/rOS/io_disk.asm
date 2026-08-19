; Naked disk trampolines into IO.SYS (PCB 1). Do not touch SP or RA.

disk_read8:
    LI T0, 1
    LI T1, 2
    JALR R0, T0, T1

disk_write8:
    LI T0, 1
    LI T1, 3
    JALR R0, T0, T1

disk_read16:
    LI T0, 1
    LI T1, 4
    JALR R0, T0, T1

disk_read32:
    LI T0, 1
    LI T1, 5
    JALR R0, T0, T1

disk_read:
    LI T0, 1
    LI T1, 6
    JALR R0, T0, T1
