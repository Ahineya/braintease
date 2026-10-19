/* Nano-X presentation boundary; no NetHack or Nano-X types cross it. */
#ifndef NETHACK_NX_UI_H
#define NETHACK_NX_UI_H
enum {
    NX_UP = -10, NX_DOWN, NX_LEFT, NX_RIGHT, NX_HOME, NX_END,
    NX_PAGEUP, NX_PAGEDOWN, NX_RESIZE, NX_CLOSE, NX_CLICK
};
struct nx_input { int key, x, y, row; };
int nhnx_open(const char *);
void nhnx_close(void);
void nhnx_clear_map(void);
void nhnx_tile(int, int, int, int);
void nhnx_focus(int, int);
void nhnx_message(const char *);
void nhnx_clear_messages(void);
void nhnx_status(int, const char *);
void nhnx_prompt(const char *);
void nhnx_overlay(const char *, const char **, int, int, int, const char *);
void nhnx_paint(void);
void nhnx_input(struct nx_input *);
int nhnx_columns(void);
int nhnx_rows(void);
#endif
