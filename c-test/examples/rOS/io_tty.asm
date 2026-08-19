; Naked IO trampolines. Do not touch SP or RA.

putchar:
    LI T0, 1
    LI T1, 0
    JALR R0, T0, T1

getchar:
    LI T0, 1
    LI T1, 1
    JALR R0, T0, T1
