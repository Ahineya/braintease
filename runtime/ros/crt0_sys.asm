; crt0 for SYS/COM images. GP, SB, SP, and FP are set by the loader.

_start:
    CALL _init_globals
    CALL main
    HALT

_exit:
    HALT
