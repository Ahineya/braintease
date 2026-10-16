#ifndef NET_HTTP_DEMO_PAGE_H
#define NET_HTTP_DEMO_PAGE_H
static const unsigned char page[] =
"<!doctype html>\n"
"<html lang=\"en\"><head><meta charset=\"us-ascii\">\n"
"<title>Ripple guest HTTP</title>\n"
"<style>body{font:20px system-ui;max-width:40em;margin:4em auto;padding:1em;"
"background:#101820;color:#e8f2f8}h1{color:#67e8b0}code{color:#91cfff}</style>"
"</head><body>\n"
"<h1>Hello from Ripple!</h1>\n"
"<p>This page was served by a freestanding C program compiled with RCC.</p>\n"
"<p>The guest owns Ethernet, ARP, IPv4, TCP and HTTP. The host only moves raw "
"Ethernet frames through the shared DEVICE_* network interface.</p>\n"
"<p>The same protocol library runs on the native target and Ripple VM, using "
"bounded buffers, explicit deadlines and one wire octet per 16-bit cell.</p>\n"
"<p>Each request gets one response with an exact Content-Length and a closed "
"connection. Reload this page to open another guest TCP connection.</p>\n"
"<p><code>GET /</code> serves this page. Unknown paths return 404 and unsupported "
"methods return 405. There is no host web server standing in for the guest.</p>\n"
"</body></html>\n";
static const unsigned char missing[] = "<!doctype html><html><body><h1>404 Not Found</h1></body></html>\n";
static const unsigned char method_error[] = "<!doctype html><html><body><h1>405 Method Not Allowed</h1></body></html>\n";
#endif
