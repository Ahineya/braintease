/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPLIX_GX_APPLICATION_H
#define RIPLIX_GX_APPLICATION_H
#include <gx-session.h>
#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>

/* Optional application identity, independent of mutable window titles.
 * Validate complete strings rather than truncating UTF-8 or application IDs.
 * This declares only the calling process; it never associates descendants. */
static int gx_application_describe(struct gx_session_record *record,
                                   const char *app_id, const char *name,
                                   const char *icon) {
    unsigned i;
    size_t id_length, name_length, icon_length;
    if (!record || !app_id || !name || !icon) {
        errno = EINVAL;
        return -1;
    }
    id_length = strlen(app_id);
    name_length = strlen(name);
    icon_length = strlen(icon);
    if (!id_length || id_length >= sizeof(record->app_id) ||
        name_length >= sizeof(record->name) || icon_length >= sizeof(record->icon)) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < id_length; i++) {
        unsigned char c = app_id[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
            errno = EINVAL;
            return -1;
        }
    }
    memset(record, 0, sizeof(*record));
    memcpy(record->app_id, app_id, id_length + 1);
    memcpy(record->name, name, name_length + 1);
    memcpy(record->icon, icon, icon_length + 1);
    return 0;
}

/* 1 registered; 0 unsupported; -1 rejected. The record receives session
 * identities on success. Callers may ignore failure and continue creating UI
 * on targets predating GX_SESSION. A probe never claims manager authority. */
static int gx_application_register(int fd, struct gx_session_record *record) {
    struct gx_session request;
    memset(&request, 0, sizeof(request));
    request.version = GXS_VERSION;
    request.operation = GXS_INFO;
    if (ioctl(fd, GX_SESSION, &request) < 0) {
        if (errno == ENOTTY || errno == ENOSYS || errno == EOPNOTSUPP)
            return 0;
        return -1;
    }
    if (request.version != GXS_VERSION || !(request.flags & GXS_CAP_REGISTRY))
        return 0;
    memset(&request, 0, sizeof(request));
    request.version = GXS_VERSION;
    request.operation = GXS_REGISTER;
    request.record = record;
    return ioctl(fd, GX_SESSION, &request) < 0 ? -1 : 1;
}
#endif
