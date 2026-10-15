#include "dns_internal.h"

int net_dns_name(const unsigned char *message, unsigned short length,
                 unsigned short offset, char *name, unsigned short *next) {
    unsigned short cursor = offset, written = 0, consumed = 0, steps = 0, count, target, i, c;
    while (cursor < length && ++steps <= length) {
        count = message[cursor++];
        if (!count) {
            if (!consumed) consumed = cursor;
            name[written] = 0; *next = consumed; return 1;
        }
        if (count >= 192) {
            if (cursor >= length) return 0;
            target = (count - 192) * 256U + message[cursor++];
            if (target < 12 || target >= cursor - 2) return 0;
            if (!consumed) consumed = cursor;
            cursor = target;
        } else {
            if (count > 63 || count > length - cursor) return 0;
            if (written) { if (written >= 253) return 0; name[written++] = '.'; }
            if (count > 253 - written) return 0;
            for (i = 0; i < count; ++i) {
                c = message[cursor++];
                /* This resolver accepts ordinary ASCII hostnames only. */
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return 0;
                name[written++] = c >= 'A' && c <= 'Z' ? c + 32 : c;
            }
        }
    }
    return 0;
}
