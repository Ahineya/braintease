/* Portable display control for RCC's native device adapter. */
#include <mmio.h>
void display_set_mode(unsigned short mode) { mmio_write(MMIO_DISP_MODE, mode); }
void display_enable(void) { mmio_write(MMIO_DISP_CTL, DISP_CTL_ENABLE); }
