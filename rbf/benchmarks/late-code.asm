; BANK_SIZE=256 MEMORY_BANKS=1
; The generator expands BENCH_PADDING into eight full banks of skipped NOPs.
; Running the short loop in bank eight exercises distant instruction scans.
; BENCH_PADDING 8 256
start:
    LI T0, 8
L_loop:
    ADDI T0, T0, -1
    BNE T0, R0, L_loop
    LI T0, 89
    STORE T0, R0, R0
    LI RV0, 0
    HALT
