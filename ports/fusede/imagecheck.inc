/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real server checks, shared by RVM and Fuse-X. No private root disk required. */
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include "nxproto.h"
#include "image-wire.h"
static volatile int check_stopping;
static void check_progress(const char *text) {if (tracing) {puts(text);fflush(stdout);}}
static void check_signal(int sig) {check_stopping = 1; signal(sig, check_signal);}
static int peer(void) {
    struct sockaddr_un address; int fd; unsigned received = 0; int got;
    nxOpenReq open_request; unsigned long ack;
    fd = socket(AF_UNIX, SOCK_STREAM, 0); CHECK(fd >= 0);
    memset(&address, 0, sizeof(address)); address.sun_family = AF_UNIX; strcpy(address.sun_path, GR_NAMED_SOCKET);
    CHECK(connect(fd, (void *)&address, sizeof(address)) == 0);
    memset(&open_request, 0, sizeof(open_request));open_request.reqType = GrNumOpen;
    open_request.length = sizeof(open_request);open_request.c16_magic = 0x4e583102UL;
    CHECK(write(fd, &open_request, sizeof(open_request)) == sizeof(open_request));
    while (received < sizeof(ack)) {
        got = read(fd, (char *)&ack + received, sizeof(ack) - received); CHECK(got > 0); received += got;
    }
    CHECK(ack == open_request.c16_magic); return fd;
}
static void reject_image(unsigned version, unsigned op, unsigned count, unsigned extra) {
    struct nxr_image_req q; int fd = peer(); fd_set reads; struct timeval wait; char byte = 0;
    memset(&q, 0, sizeof(q));q.header.reqType = NXR_IMAGE;q.header.length = sizeof(q) + extra;
    q.version = version;q.op = op;q.count = count;
    CHECK(write(fd, &q, sizeof(q)) == sizeof(q));
    if (extra) CHECK(write(fd, &byte, 1) == 1);
    FD_ZERO(&reads);FD_SET(fd, &reads);wait.tv_sec = 3;wait.tv_usec = 0;
    CHECK(select(fd + 1, &reads, NULL, NULL, &wait) > 0);
    CHECK(read(fd, &byte, 1) == 0);close(fd);
}
static int foreign_check(int argc, char **argv) {
    FDE_IMAGE id, own, leaked[3]; GR_WINDOW_ID target, own_window; GR_GC_ID foreign_gc, own_gc;
    struct fde_rect r = {0,0,0,0,1,1,1,1}; unsigned short pixel = 0xffff; unsigned i;
    CHECK(argc == 5);id = strtoul(argv[2], NULL, 10);target = strtoul(argv[3], NULL, 10);foreign_gc = strtoul(argv[4], NULL, 10);
    CHECK(GrOpen() >= 0);
    own = fde_image_load(1, 1, &pixel, NULL); CHECK(own);
    own_gc = GrNewGC(); own_window = GrNewWindow(GR_ROOT_WINDOW_ID, 0, 0, 1, 1, 0, 0, 0);
    CHECK(own_gc && own_window);
    CHECK(fde_image_destroy(id) < 0);
    CHECK(fde_image_draw(id, own_window, own_gc, &r, 1, 0) < 0);
    CHECK(fde_image_draw(own, target, own_gc, &r, 1, 0) < 0);
    CHECK(fde_image_draw(own, own_window, foreign_gc, &r, 1, 0) < 0);
    for (i = 0; i < 3; ++i) {leaked[i] = fde_image_create(1, 1, 0);CHECK(leaked[i]);}
    /* Intentionally no destroy/GrClose: server EOF cleanup must reclaim all. */
    _exit(0); return 0;
}
static int image_checks(char *program) {
    unsigned short pixels[51], mask[6] = {0,0,0,0,0,0}, one = 0xffff;
    FDE_IMAGE id, extra[3], stale;
    GR_WINDOW_ID pixmaps[13];
    struct fde_rect rects[2] = {{0,0,0,0,17,3,17,3},{0,0,0,0,18,3,17,3}};
    struct fde_rect sample = {-3,4,0,0,17,3,51,9};
    GR_WM_PROPERTIES props; GR_REGION_ID region; GR_RECT clip = {12,30,90,20};
    GR_SCREEN_INFO screen; int x, y, child, result; unsigned i;
    char id_text[20], target_text[20], gc_text[20];
    window = GrNewWindow(GR_ROOT_WINDOW_ID, 20, 20, 128, 96, 0, 0, 0);gc = GrNewGC();CHECK(window && gc);
    memset(&props, 0, sizeof(props));props.flags = GR_WM_FLAGS_PROPS | GR_WM_FLAGS_TITLE;
    props.props = GR_WM_PROPS_NODECORATE | GR_WM_PROPS_NOAUTOMOVE;props.title = "FuseDE pixel check";
    GrSetWMProperties(window, &props);GrMapWindow(window);
    check_progress("FDE_CHECK_STARTED");
    for (y = 0; y < 3; ++y) for (x = 0; x < 17; ++x) {
        pixels[y * 17 + x] = 0x8000 + y * 17 + x;
        if ((x + y) % 3) mask[y * 2 + x / 16] |= 0x8000U >> (x % 16);
    }
    CHECK(!fde_image_create(512, 512, FDE_IMAGE_MASK));
    id = fde_image_create(17, 3, FDE_IMAGE_MASK);CHECK(id);
    CHECK(fde_image_draw(id, window, gc, rects, 1, 0) < 0 && fde_image_status() == NXR_IMAGE_INCOMPLETE);
    CHECK(fde_image_upload(id, 1, pixels, 1) < 0);
    CHECK(fde_image_upload(id, 0, pixels, 1) == 0);
    CHECK(fde_image_upload(id, 0, pixels, 1) < 0);
    CHECK(fde_image_upload(id, 1, pixels + 1, 50) == 0);
    CHECK(fde_image_upload(id, 51, mask, 6) == 0);
    CHECK(fde_image_upload(id, 57, pixels, 1) < 0);
    check_progress("FDE_CHECK_UPLOADS");
    for (i = 0; i < 3; ++i) {extra[i] = fde_image_create(1, 1, 0);CHECK(extra[i]);}
    CHECK(!fde_image_create(1, 1, 0) && fde_image_status() == NXR_IMAGE_QUOTA);
    for (i = 0; i < 3; ++i) CHECK(fde_image_destroy(extra[i]) == 0);
    /* Images and public pixmaps share the 16-object global quota. */
    for (i = 0; i < 13; ++i) {pixmaps[i] = GrNewPixmapEx(1, 1, MWIF_RGB565, NULL);CHECK(pixmaps[i]);}
    for (i = 0; i < 2; ++i) {extra[i] = fde_image_create(1, 1, 0);CHECK(extra[i]);}
    CHECK(!fde_image_create(1, 1, 0) && fde_image_status() == NXR_IMAGE_QUOTA);
    for (i = 0; i < 13; ++i) GrDestroyWindow(pixmaps[i]);
    for (i = 0; i < 2; ++i) CHECK(fde_image_destroy(extra[i]) == 0);
    stale = fde_image_load(1, 1, &one, NULL);CHECK(stale && fde_image_destroy(stale) == 0);
    extra[0] = fde_image_load(1, 1, &one, NULL);CHECK(extra[0] && stale != extra[0]);
    CHECK(fde_image_destroy(stale) < 0);CHECK(fde_image_destroy(extra[0]) == 0);
    check_progress("FDE_CHECK_QUOTAS");
    sprintf(id_text, "%lu", id);sprintf(target_text, "%lu", (unsigned long)window);sprintf(gc_text, "%lu", (unsigned long)gc);
    for (i = 0; i < 5; ++i) {
        child = fork();CHECK(child >= 0);
        if (!child) {char *args[] = {program,"--foreign",id_text,target_text,gc_text,NULL};execv("/bin/fdeshowcase",args);_exit(2);}
        CHECK(waitpid(child, &result, 0) == child && result == 0);GrGetScreenInfo(&screen);
        check_progress("FDE_CHECK_FOREIGN");
    }
    {
        FILE *file; FDE_IMAGE loaded;
        static const unsigned char fixture[] = {'F','D','I','1',1,0,1,0,0,0,0,0,0xff,0xff,0};
        CHECK(!fde_image_load_file("/tmp/fde-no-such-file") && fde_image_status() == 5);
        for (i = 0; i < 5; ++i) {
            file = fopen("/tmp/fde-bad", "wb");CHECK(file);
            CHECK(fwrite(fixture, 1, 13, file) == 13 && fclose(file) == 0);
            CHECK(!fde_image_load_file("/tmp/fde-bad") && fde_image_status() == NXR_IMAGE_INVALID);
        }
        file = fopen("/tmp/fde-bad", "wb");CHECK(file);
        CHECK(fwrite(fixture, 1, 15, file) == 15 && fclose(file) == 0);
        CHECK(!fde_image_load_file("/tmp/fde-bad"));
        file = fopen("/tmp/fde-bad", "wb");CHECK(file);
        CHECK(fwrite(fixture, 1, 14, file) == 14 && fclose(file) == 0);
        loaded = fde_image_load_file("/tmp/fde-bad");CHECK(loaded && fde_image_destroy(loaded) == 0);
        CHECK(unlink("/tmp/fde-bad") == 0);
    }
    check_progress("FDE_CHECK_FILES");
    reject_image(2, NXR_IMAGE_CREATE, 0, 0);
    reject_image(1, 99, 0, 0);
    reject_image(1, NXR_IMAGE_UPLOAD, 1, 0);
    reject_image(1, NXR_IMAGE_UPLOAD, NXR_IMAGE_CHUNK + 1, 0);
    reject_image(1, NXR_IMAGE_DRAW, 1, 1);
    check_progress("FDE_CHECK_REJECTIONS");
    GrSetGCForeground(gc, GR_RGB(24,32,40));GrFillRect(window, gc, 0, 0, 128, 96);
    /* A bad second rectangle must leave the first one entirely unpainted. */
    CHECK(fde_image_draw(id, window, gc, rects, 2, 0) < 0);
    CHECK(fde_image_draw(id, window, gc, &sample, 1, 0) == 0);
    theme_image = fde_image_load_file("/usr/share/fusede/themes/slate/theme.fdi");CHECK(theme_image);
    icon_image = fde_image_load_file("/usr/share/fusede/icons/lucide/icons.fdi");CHECK(icon_image);
    region = GrNewRegion();CHECK(region);GrUnionRectWithRegion(region, &clip);GrSetGCRegion(gc, region);
    skin(FDE_ASSET_BUTTON_NORMAL, 8, 28, 101, 27);
    GrSetGCRegion(gc, 0);GrDestroyRegion(region);
    icon(FDE_ICON_FOLDER, 1, 12, 62, GR_RGB(80,132,155));
    /* Ensure image drawing restores the user's GC foreground for ordinary API calls. */
    GrFillRect(window, gc, 110, 80, 8, 8);
    GrFlush();GrGetScreenInfo(&screen);
    signal(SIGUSR1, check_signal);
    puts("FDE_IMAGE_CHECKS_OK");fflush(stdout);
    while (!check_stopping) usleep(10000);
    CHECK(fde_image_destroy(icon_image) == 0 && fde_image_destroy(theme_image) == 0 && fde_image_destroy(id) == 0);
    GrDestroyWindow(window);GrDestroyGC(gc);GrClose();puts("FDE_IMAGE_CHECKS_CLOSED");return 0;
}
