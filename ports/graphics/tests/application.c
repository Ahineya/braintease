/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the production optional client against old, absent and rejecting
 * session services. Never expose child attachment or manager operations. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <gx-session.h>
static int scenario, probes, registrations;
static int session_ioctl(int fd, unsigned code, void *value) {
    struct gx_session *request = value;
    assert(fd == 7 && code == GX_SESSION);
    assert(request->version == GXS_VERSION);
    assert(!request->authority && !request->object && !request->revision &&
           !request->value && !request->flags && !request->reserved);
    if (request->operation == GXS_INFO) {
        probes++;
        assert(!request->record);
        if (scenario >= 1 && scenario <= 4) {
            errno = scenario == 1 ? ENOTTY : scenario == 2 ? ENOSYS :
                    scenario == 3 ? EOPNOTSUPP : EIO;
            return -1;
        }
        request->flags = scenario == 5 ? 0 : GXS_CAP_REGISTRY;
        if (scenario == 6)
            request->version++;
        return 0;
    }
    assert(request->operation == GXS_REGISTER);
    assert(request->record);
    registrations++;
    assert(!strcmp(request->record->app_id, "org.riplix.Terminal"));
    assert(!strcmp(request->record->name, "Термінал"));
    assert(!strcmp(request->record->icon, "terminal"));
    if (scenario == 7) {
        errno = ENOMEM;
        return -1;
    }
    request->record->process = 101;
    request->record->instance = 202;
    return 0;
}
#define ioctl session_ioctl
#include <gx-application.h>
#undef ioctl

int main(void) {
    struct gx_session_record record, before;
    char long_name[65], long_icon[129], limit_id[64];
    int old_registrations;
    memset(long_name, 'n', sizeof(long_name));
    long_name[sizeof(long_name) - 1] = 0;
    memset(long_icon, 'i', sizeof(long_icon));
    long_icon[sizeof(long_icon) - 1] = 0;
    memset(limit_id, 'a', sizeof(limit_id));
    limit_id[sizeof(limit_id) - 1] = 0;
    assert(!gx_application_describe(&record, limit_id, "Name", "file"));
    assert(strlen(record.app_id) == 63);
    before = record;
    assert(gx_application_describe(&record, "", "Name", "file") == -1 && errno == EINVAL);
    assert(!memcmp(&before, &record, sizeof(record)));
    assert(gx_application_describe(&record, "app/child", "Name", "file") == -1);
    assert(gx_application_describe(&record, "org.riplix.Test", long_name, "file") == -1);
    assert(gx_application_describe(&record, "org.riplix.Test", "Name", long_icon) == -1);
    assert(gx_application_describe(&record, NULL, "Name", "file") == -1);
    assert(gx_application_describe(NULL, "app", "Name", "file") == -1);
    assert(!memcmp(&before, &record, sizeof(record)));
    assert(!gx_application_describe(&record, "org.riplix.Terminal", "Термінал", "terminal"));
    for (scenario = 1; scenario <= 6; scenario++) {
        old_registrations = registrations;
        assert(gx_application_register(7, &record) == (scenario == 4 ? -1 : 0));
        assert(registrations == old_registrations);
        assert(!record.process && !record.instance);
    }
    scenario = 7;
    assert(gx_application_register(7, &record) == -1 && errno == ENOMEM);
    assert(!record.process && !record.instance);
    scenario = 0;
    assert(gx_application_register(7, &record) == 1);
    assert(record.process == 101 && record.instance == 202);
    assert(probes == 8 && registrations == 2);
    puts("GX_APPLICATION passed: bounds, stable metadata, old-kernel fallback, rejection, identities");
    return 0;
}
