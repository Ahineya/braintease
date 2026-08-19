; Must be the first object in IO.SYS. Slot k is instruction k.
; JAL R0 does not clobber RA, so the real function returns to the caller.

io_putchar_vec:
    JAL R0, R0, putchar
io_getchar_vec:
    JAL R0, R0, getchar
io_disk_read8_vec:
    JAL R0, R0, io_disk_read8
io_disk_write8_vec:
    JAL R0, R0, io_disk_write8
io_disk_read16_vec:
    JAL R0, R0, io_disk_read16
io_disk_read32_vec:
    JAL R0, R0, io_disk_read32
io_disk_read_vec:
    JAL R0, R0, io_disk_read
