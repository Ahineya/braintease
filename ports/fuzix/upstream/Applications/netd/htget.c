/* FUZIX htget command, with the Ripple HTTP/TLS implementation.
 * The generic client lives in ports/tls; its ordinary file-download CLI and
 * independent streaming worker share the same bounded HTTP parser. */
#include <ripple_http.h>
int main(int argc,char **argv) { return ripple_http_main(argc,argv); }
