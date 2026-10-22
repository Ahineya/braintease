/* SPDX-License-Identifier: GPL-2.0-only */
/* Host policy harness: include the production kernel session adapter unchanged.
 * Host pointer/long sizes intentionally do not test C16 wire marshalling. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gx.h>
#include <gx-session.h>
#include <gx-panel.h>

#define PTABSIZE 8
#define P_ZOMBIE 7
struct p_tab {
    unsigned p_status;
    unsigned p_pid;
    struct p_tab *p_pptr;
};
typedef struct p_tab *ptptr;
static struct p_tab ptab[PTABSIZE];
static struct {
    ptptr u_ptab;
    unsigned u_euid;
    unsigned u_error;
} udata;
static unsigned long host_revision;
static unsigned global_wakes, checks, info_failures, exit_calls;
static unsigned panel_calls,panel_revokes,panel_failures;

static void check(int passed, const char *expression, unsigned line) {
    ++checks;
    if (!passed) {
        fprintf(stderr, "SESSION_POLICY_FAILED line %u: %s\n", line, expression);
        exit(1);
    }
}
#define CHECK(expression) check(!!(expression), #expression, __LINE__)

static int fail(unsigned error) {
    udata.u_error = error;
    return -1;
}
static int uget(const void *source, void *destination, size_t length) {
    memcpy(destination, source, length);
    return 0;
}
static int uput(const void *source, void *destination, size_t length) {
    memcpy(destination, source, length);
    return 0;
}
static int valaddr_w(void *address, size_t length, size_t minimum) {
    return address != NULL && length >= minimum;
}
static int valaddr_r(const void *address, size_t length, size_t minimum) {
    return address != NULL && length >= minimum;
}
static void *mine(unsigned long handle) {
    if (handle == 1 && udata.u_ptab == &ptab[0]) return &ptab[0];
    fail(EPERM);
    return NULL;
}
static long rcc_hosted_gx(unsigned operation, unsigned long *args) {
    CHECK(operation == 31 || operation == 32);
    if (operation == 32) {
        ++panel_calls;
        if (panel_failures) { --panel_failures; return -EIO; }
        if (args[0] == 64) { ++panel_revokes; CHECK(args[1] != 0); }
        if (args[0] == GXP_INFO) args[3] = GXP_CAP_OVERLAY;
        args[8] = 1280; args[9] = 800;
        return 0;
    }
    if (args[0] == GXS_INFO && info_failures) {
        --info_failures;
        return -EIO;
    }
    if (args[0] == GXS_INFO) {
        args[3] = 2;
        args[5] = 1;
        args[7] = GXS_CAP_REGISTRY | GXS_CAP_CONTROL | GXS_CAP_WATCH | GXS_CAP_ATTACH_CHILD;
    } else if (args[0] == 64) {
        ++host_revision;
    } else if (args[0] == 65) {
        ++exit_calls;
    }
    args[4] = host_revision;
    return 0;
}
static long host(unsigned operation, unsigned long *args) {
    long result = rcc_hosted_gx(operation, args);
    if (result < 0) udata.u_error = (unsigned)-result;
    return result;
}

#include "../session-abi.h"
#include "../panel-abi.h"

static void reset(void) {
    memset(ptab, 0, sizeof(ptab));
    memset(&udata, 0, sizeof(udata));
    memset(session_processes, 0, sizeof(session_processes));
    session_serial = manager_serial = manager_token = 0;
    session_notified_revision = 0;
    manager_owner = NULL;
    manager_pid = 0;
    host_revision = 10;
    global_wakes = info_failures = exit_calls = 0;
    panel_calls = panel_revokes = panel_failures = 0;
    ptab[0].p_status = ptab[1].p_status = 1;
    ptab[0].p_pid = 20;
    ptab[1].p_pid = 21;
    udata.u_ptab = &ptab[0];
}
static struct gx_session request(unsigned operation) {
    struct gx_session value;
    memset(&value, 0, sizeof(value));
    value.version = GXS_VERSION;
    value.operation = operation;
    return value;
}
static struct gx_session watch(unsigned process, unsigned long revision) {
    struct gx_session value = request(GXS_WATCH);
    udata.u_ptab = &ptab[process];
    value.revision = (uint32_t)revision;
    CHECK(session_ioctl((char *)&value) == 0);
    CHECK(value.revision == host_revision);
    return value;
}
static void poll_session(void) {
    /* Same notification condition used by ripple_gfx_poll(). */
    if (session_any_changed()) ++global_wakes;
}

static void watch_notifications(void) {
    struct gx_session value;
    unsigned i;
    reset();
    CHECK(watch(0, host_revision).flags == 0);
    CHECK(watch(1, host_revision).flags == 0);
    CHECK(!session_own_changed());
    for (i = 0; i < 100; ++i) poll_session();
    CHECK(global_wakes == 0);

    /* Process zero abandons its subscription (including closing /dev/gfx).
     * Subscriptions are process-scoped, so it remains behind indefinitely. */
    ++host_revision;
    poll_session();
    CHECK(global_wakes == 1);
    udata.u_ptab = &ptab[1];
    for (i = 0; i < 100; ++i) {
        poll_session();
        CHECK(global_wakes == 1);
        CHECK(session_own_changed());
    }
    CHECK(watch(1, host_revision).flags == 0);
    CHECK(!session_own_changed());
    for (i = 0; i < 100; ++i) {
        poll_session();
        CHECK(global_wakes == 1);
        CHECK(!session_own_changed());
    }
    CHECK(session_find(&ptab[0], 0)->seen == 10);

    ++host_revision;
    poll_session();
    CHECK(global_wakes == 2);
    for (i = 0; i < 100; ++i) {
        poll_session();
        CHECK(global_wakes == 2);
        CHECK(session_own_changed());
    }
    /* WATCH acknowledges its input revision, never the newer output revision. */
    value = watch(1, host_revision - 1);
    CHECK(value.flags == GXS_CHANGED);
    CHECK(session_find(&ptab[1], 0)->seen == host_revision - 1);
    CHECK(session_own_changed());
    CHECK(watch(1, host_revision).flags == 0);
    for (i = 0; i < 100; ++i) {
        poll_session();
        CHECK(global_wakes == 2);
        CHECK(!session_own_changed());
    }
    session_exit(&ptab[0]);
    session_exit(&ptab[1]);
    CHECK(exit_calls == 2);
    CHECK(!session_find(&ptab[0], 0));
    CHECK(!session_find(&ptab[1], 0));
    CHECK(!session_any_changed());
    puts("SESSION_POLICY_CHECK watch-coalescing-and-independent-level-readiness");
}

static void manager_release_failure(void) {
    struct gx_session value;
    unsigned long token;
    reset();
    value = request(GXS_CLAIM_MANAGER);
    CHECK(session_ioctl((char *)&value) == 0);
    token = value.authority;
    CHECK(token != 0);
    CHECK(manager_owner == &ptab[0]);
    CHECK(manager_pid == ptab[0].p_pid);
    CHECK(manager_token == token);

    value = request(GXS_RELEASE_MANAGER);
    value.authority = (uint32_t)token;
    info_failures = 1;
    CHECK(session_ioctl((char *)&value) == -1);
    CHECK(udata.u_error == EIO);
    CHECK(info_failures == 0);
    CHECK(value.authority == token);
    CHECK(manager_owner == &ptab[0]);
    CHECK(manager_pid == ptab[0].p_pid);
    CHECK(manager_token == token);

    udata.u_error = 0;
    panel_failures = 1;
    CHECK(session_ioctl((char *)&value) == -1);
    CHECK(udata.u_error == EIO);
    CHECK(manager_token == token);
    CHECK(manager_owner == &ptab[0]);
    CHECK(value.authority == token);
    CHECK(session_ioctl((char *)&value) == 0);
    CHECK(panel_revokes == 1);
    CHECK(value.authority == 0);
    CHECK(manager_owner == NULL);
    CHECK(manager_pid == 0);
    CHECK(manager_token == 0);
    value = request(GXS_RELEASE_MANAGER);
    value.authority = (uint32_t)token;
    CHECK(session_ioctl((char *)&value) == -1);
    CHECK(udata.u_error == EPERM);
    session_exit(&ptab[0]);
    CHECK(exit_calls == 1);
    puts("SESSION_POLICY_CHECK manager-release-failure-is-atomic");
}

static void panel_authority(void) {
    struct gx_session manager;
    struct gx_panel panel;
    struct gx_panel_rect region = {8, 8, 100, 32};
    unsigned calls;
    reset();
    memset(&panel, 0, sizeof(panel));
    panel.version = GXP_VERSION;
    panel.operation = GXP_INFO;
    CHECK(panel_ioctl((char *)&panel) == 0);
    CHECK(panel.flags == GXP_CAP_OVERLAY);
    CHECK(panel.width == 1280 && panel.height == 800);
    CHECK(!session_find(udata.u_ptab, 0));
    manager = request(GXS_CLAIM_MANAGER);
    CHECK(session_ioctl((char *)&manager) == 0);
    memset(&panel, 0, sizeof(panel));
    panel.version = GXP_VERSION;
    panel.operation = GXP_CONFIGURE;
    panel.authority = manager.authority;
    panel.window = 1;
    panel.count = 1;
    panel.regions = &region;
    panel.top = 40;
    panel.bottom = 96;
    CHECK(panel_ioctl((char *)&panel) == 0);
    calls = panel_calls;
    panel.authority = 0;
    CHECK(panel_ioctl((char *)&panel) == -1 && udata.u_error == EPERM);
    CHECK(panel_calls == calls);
    panel.authority = manager.authority;
    udata.u_ptab = &ptab[1];
    CHECK(panel_ioctl((char *)&panel) == -1 && udata.u_error == EPERM);
    CHECK(panel_calls == calls);
    udata.u_ptab = &ptab[0];
    panel.window = 2;
    CHECK(panel_ioctl((char *)&panel) == -1 && udata.u_error == EPERM);
    panel.window = 1;
    panel.flags = GXP_MODAL;
    CHECK(panel_ioctl((char *)&panel) == -1 && udata.u_error == EINVAL);
    panel.flags = GXP_MODAL | GXP_KEYBOARD;
    panel.count = 0;
    CHECK(panel_ioctl((char *)&panel) == -1 && udata.u_error == EINVAL);
    panel.count = 1;
    panel.regions = NULL;
    CHECK(panel_ioctl((char *)&panel) == -1 && udata.u_error == EINVAL);
    CHECK(panel_calls == calls);
    panel.regions = &region;
    CHECK(panel_ioctl((char *)&panel) == 0);
    panel.flags = panel.count = panel.top = panel.bottom = 0;
    panel.regions = NULL;
    CHECK(panel_ioctl((char *)&panel) == 0);
    panel.count = 1;
    panel.regions = &region;
    panel.flags = GXP_KEYBOARD | GXP_MODAL;
    {
        struct gx_panel_upload upload;
        uint8_t pixels[4]={0,0,0,128};
        memset(&upload,0,sizeof(upload));
        upload.version=GXP_VERSION;upload.authority=manager.authority;upload.window=1;
        upload.width=upload.height=1;upload.stride=4;upload.pixels=pixels;
        CHECK(panel_upload_ioctl((char *)&upload)==0);
        calls=panel_calls;
        udata.u_ptab=&ptab[1];
        CHECK(panel_upload_ioctl((char *)&upload)==-1&&udata.u_error==EPERM);
        CHECK(panel_calls==calls);
        udata.u_ptab=&ptab[0];upload.stride=3;
        CHECK(panel_upload_ioctl((char *)&upload)==-1&&udata.u_error==EINVAL);
        upload.stride=0xffffffffUL;upload.height=2;
        CHECK(panel_upload_ioctl((char *)&upload)==-1&&udata.u_error==EINVAL);
        CHECK(panel_calls==calls);
    }
    calls = panel_calls;
    panel.operation = GXP_RELEASE;
    CHECK(panel_ioctl((char *)&panel) == -1 && udata.u_error == EINVAL);
    CHECK(panel_calls == calls);
    panel.flags = panel.count = panel.top = panel.bottom = 0;
    panel.regions = NULL;
    CHECK(panel_ioctl((char *)&panel) == 0);
    manager.operation = GXS_RELEASE_MANAGER;
    manager.flags = 0;
    CHECK(session_ioctl((char *)&manager) == 0);
    calls = panel_calls;
    CHECK(panel_ioctl((char *)&panel) == -1 && udata.u_error == EPERM);
    CHECK(panel_calls == calls);
    CHECK(panel_revokes == 1);
    puts("SESSION_POLICY_CHECK panel-manager-token-window-ownership-and-validation");
}

int main(void) {
    watch_notifications();
    manager_release_failure();
    panel_authority();
    printf("SESSION_POLICY_PASSED %u\n", checks);
    return 0;
}
