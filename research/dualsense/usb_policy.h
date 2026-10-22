#ifndef DUALSENSE_USB_POLICY_H
#define DUALSENSE_USB_POLICY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool ds_query_allowed(uint8_t group, uint8_t command) {
    return (group == 3 && command == 3) || (group == 12 && command == 2) ||
           (group == 5 && (command == 2 || command == 4)) ||
           (group == 1 && (command == 9 || command == 17)) ||
           (group == 0x70 && command == 1);
}

/* IOKit report types: output=1, feature=2. No general-purpose write escape. */
static inline bool ds_send_allowed(unsigned type, const uint8_t *data, size_t count) {
    if (!data) return false;
    if (type == 2) {
        if (count < 3 || count > 64 || data[0] != 0x80 || !ds_query_allowed(data[1], data[2])) return false;
        /* Sony's six-chunk telemetry request, not a generic factory selector. */
        if (data[1] == 0x70 && count != 64) return false;
        for (size_t i = 3; i < count; ++i) if (data[i]) return false;
        return true;
    }
    if (type != 1 || count != 48 || data[0] != 2) return false;
    if (data[1] == 0 && data[2] == 4) {
        for (size_t i = 3; i < 45; ++i) if (data[i]) return false;
        return data[45] <= 16 && data[46] <= 16 && data[47] <= 16;
    }
    if (data[1] == 3 && data[2] == 0 && data[3] <= 24 && data[4] <= 24) {
        for (size_t i = 5; i < count; ++i) if (data[i]) return false;
        return true;
    }
    return false;
}

static inline bool ds_get_allowed(uint8_t id, size_t count) {
    if ((id == 0x0b || id == 0x0c) && count == 42) return true;
    return count >= 1 && count <= 64 && (id == 5 || id == 0x20 || id == 0x81);
}
#endif
