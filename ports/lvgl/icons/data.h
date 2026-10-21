/* Lucide artwork: ISC / Feather MIT; see LICENSE.Lucide.txt. */
#ifndef GRAPHITE_ICON_DATA_H
#define GRAPHITE_ICON_DATA_H
#include "lvgl.h"
extern const lv_font_t ui_icons_16, ui_icons_32;
const char *ui_icon_text(const char *name);
unsigned ui_icon_count(void);
const char *ui_icon_name(unsigned index);
#endif
