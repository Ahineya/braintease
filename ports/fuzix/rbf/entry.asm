; SPDX-License-Identifier: GPL-2.0-only
; Physical boot profile: kernel GP=1, kernel stack=2, init argument RAM=3.
_start:
    LI GP, 1
    LI SB, 2
    LI SP, 1
    LI FP, 1
    CALL _init_globals
    CALL main
    HALT
_exit:
    HALT
rbf_stop:
    ADD RV0, A0, R0
    HALT
; IRQ polling is absent from this boot CPU. No asynchronous driver is linked.
__hard_di:
    LI RV0, 0
    RET
__hard_irqrestore:
__hard_ei:
    RET
