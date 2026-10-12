; Small RGB565 animation executed entirely by the Brainfuck CPU.
; BANK_SIZE=512, MEMORY_BANKS=1. No C startup, stack or external objects.
; 16x12 pixels: front 32..223, back 224..415, NEXT_TICK descriptor 416..423.
; Frames 1/2 initialize both buffers. Frame N>=3 has a white two-pixel
; vertical marker at x=(N-3)%16, y=5/6 on red/green/blue horizontal bands.
; The back buffer contains frame N-2, so erase the marker two positions behind.

_start:
    LI T0, 4108
    LI T1, 16
    STORE T0, R0, T1          ; Resolution 16x12.
    LI T0, 3
    LI T1, 6
    STORE T0, R0, T1          ; Legacy RGB565.
    LI T7, 2

_initialize_back:
    LI T1, 224
    LI T0, 63488              ; Red.
    LI T2, 288
_red:
    STORE T0, R0, T1
    ADDI T1, T1, 1
    BNE T1, T2, _red
    LI T0, 2016               ; Green.
    LI T2, 352
_green:
    STORE T0, R0, T1
    ADDI T1, T1, 1
    BNE T1, T2, _green
    LI T0, 31                 ; Blue.
    LI T2, 416
_blue:
    STORE T0, R0, T1
    ADDI T1, T1, 1
    BNE T1, T2, _blue
    LI T0, 1
    LI T1, 9
    STORE T0, R0, T1          ; Publish a complete frame immediately.
    ADDI T7, T7, -1
    BNE T7, R0, _initialize_back

    LI T0, 1
    LI T1, 416
    STORE T0, R0, T1          ; Device ABI version.
    LI T0, 8
    LI T1, 417
    STORE T0, R0, T1          ; Descriptor length.
    LI T0, 416
    LI T1, 28
    STORE T0, R0, T1          ; Descriptor in bank 0.
    LI T4, 0                 ; New marker column.
    LI T5, 14                ; Previous marker in this back buffer.
    LI T6, 65535              ; White.
    LI T7, 2016               ; Background green.

_animate:
    ADDI T0, T5, 304
    STORE T7, R0, T0
    ADDI T0, T0, 16
    STORE T7, R0, T0
    ADDI T0, T4, 304
    STORE T6, R0, T0
    ADDI T0, T0, 16
    STORE T6, R0, T0
    LI T0, 1
    LI T1, 9
    STORE T0, R0, T1          ; Swap front and back.

    LI T0, 3
    LI T1, 29
    STORE T0, R0, T1          ; NEXT_TICK: host pacing never accrues catch-up debt.
    ADDI T4, T4, 1
    ANDI T4, T4, 15
    ADDI T5, T5, 1
    ANDI T5, T5, 15
    BEQ R0, R0, _animate      ; Keep the window alive until the host closes it.
