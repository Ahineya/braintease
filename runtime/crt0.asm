; crt0.asm - C Runtime Startup Code
; This is the entry point for C programs
; It initializes the runtime environment and calls main

; Entry point - this is where execution begins
_start:
    ; Initialize stack pointer
    LI SB, __rcc_layout_stack_bank ; Linker places the stack after all global banks
    LI SP, 1     ; Stack pointer starts at the start of the bank (SP/R29) and grows upwards
    LI FP, 1     ; Frame pointer starts at the start of the bank (FP/R30) and grows upwards
    ; Initialize global pointer
    LI GP, 1        ; Global pointer starts at 0 (GP/R31)
    
    ; TODO: Zero BSS section (uninitialized globals)
    ; For now, we'll skip this as we don't have a BSS section yet
    
    ; Initialize global variables
    ; Call _init_globals if it exists (the linker will resolve this)
    CALL _init_globals
    
    ; Call main function
    ; main() should be provided by the user program
    LI X2, 0      ; no incoming stack arguments
    CALL main
    
    ; If main returns, halt the program
    ; In a real system, we might call exit() here
    HALT

; Exit function - terminates the program
_exit:
    ; For now, just halt
    HALT

; First heap bank follows globals and the stack, retaining bank 5 for small
; programs. The return value uses the ordinary two-cell pointer ABI.
__rcc_heap_start:
    LI RV0, 0
    LI RV1, __rcc_layout_heap_bank
    RET
