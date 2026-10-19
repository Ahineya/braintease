/* C16 browser transport limits. No host sockets or HTTP implementation. */
#ifndef RIPPLE_BROWSER_H
#define RIPPLE_BROWSER_H
#define RIPPLE_BROWSER_PAGE_LIMIT 65536UL
void ripple_browser_init(void);
void ripple_browser_screen_init(void);
void ripple_browser_begin(void);
int ripple_browser_read(int, void *, unsigned);
int ripple_browser_write(int, const void *, unsigned);
int ripple_browser_limit_hit(void);
int ripple_browser_secure(int, const char *);
void ripple_browser_close(int);
void ripple_browser_tls_close(void);
#endif
