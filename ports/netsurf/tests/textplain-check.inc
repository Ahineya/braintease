/* SPDX-License-Identifier: GPL-2.0-only */
/* Numeric octets exercise the same C16 file ABI as host-mounted UTF-8. */
static const ns_octet_t plain_fixture[] = {
    208, 154, 208, 190, 208, 187, 208, 184, 32, 208, 191, 209, 150, 208, 180, 209, 130, 209, 128, 208,
    184, 208, 188, 208, 186, 208, 176, 32, 208, 191, 208, 190, 209, 130, 209, 128, 209, 150, 208, 177,
    208, 189, 208, 176, 32, 208, 190, 208, 177, 208, 190, 208, 188, 58, 32, 208, 163, 208, 186, 209,
    128, 208, 176, 209, 151, 208, 189, 208, 176, 32, 209, 151, 32, 209, 148, 32, 210, 145, 32, 208,
    134, 32, 208, 135, 32, 208, 132, 32, 210, 144, 46, 32, 208, 191, 209, 128, 208, 190, 208, 179,
    209, 128, 208, 176, 208, 188, 209, 131, 208, 178, 208, 176, 208, 189, 208, 189, 209, 143, 208, 191,
    209, 128, 208, 190, 208, 179, 209, 128, 208, 176, 208, 188, 209, 131, 208, 178, 208, 176, 208, 189,
    208, 189, 209, 143, 208, 191, 209, 128, 208, 190, 208, 179, 209, 128, 208, 176, 208, 188, 209, 131,
    208, 178, 208, 176, 208, 189, 208, 189, 209, 143, 208, 191, 209, 128, 208, 190, 208, 179, 209, 128,
    208, 176, 208, 188, 209, 131, 208, 178, 208, 176, 208, 189, 208, 189, 209, 143, 208, 191, 209, 128,
    208, 190, 208, 179, 209, 128, 208, 176, 208, 188, 209, 131, 208, 178, 208, 176, 208, 189, 208, 189,
    209, 143, 208, 191, 209, 128, 208, 190, 208, 179, 209, 128, 208, 176, 208, 188, 209, 131, 208, 178,
    208, 176, 208, 189, 208, 189, 209, 143, 32, 208, 186, 209, 150, 208, 189, 208, 181, 209, 134, 209,
    140,
};
static void plain_encoding_test(struct browser_window *bw)
{
    static const ns_octet_t expected[] = {195,128,195,169};
    static const ns_octet_t breaks[] = {208,132,208,134,209,151,210,145};
    static const ns_octet_t replacement[] = {65,239,191,189,66};
    const char *pages[] = {
        "data:text/plain;charset=windows-1252,%C0%E9",
        "data:text/plain,%D0%84%0D%0A%D0%86%0A%0D%D1%97%09%D2%91",
        "data:text/plain,A%EF%BF%BDB"
    };
    const ns_octet_t *wanted[] = {expected, breaks, replacement};
    const size_t lengths[] = {sizeof(expected), sizeof(breaks), sizeof(replacement)};
    int i;
    struct rect clip = { 0, 0, 480, 8000 };
    struct redraw_context redraw = { .interactive = false, .background_images = true, .plot = &plotters };
    for (i = 0; i < 3; i++) {
        nsurl *url = NULL;
        check(nsurl_create(pages[i], &url) == NSERROR_OK, "plain encoding URL");
        if (!url) continue;
        check(browser_window_navigate(bw, url, NULL, BW_NAVIGATE_HISTORY, NULL, NULL, NULL) == NSERROR_OK,
              "plain encoding navigation");
        nsurl_unref(url);
        check(load_finished(bw), "plain encoding loaded");
        viewport = 480; browser_window_reformat(bw, false, viewport, 320);
        plain_length = 0; plain_valid = true; capture_plain = true;
        check(browser_window_redraw(bw, 0, 0, &clip, &redraw), "plain encoding redraw");
        capture_plain = false;
        check(plain_valid && plain_length == lengths[i] && !memcmp(plain_drawn, wanted[i], lengths[i]),
              "explicit charset, CRLF/LFCR/tabs and replacement character");
    }
}
static void plain_metrics_test(void)
{
    /* e + combining acute, Ukrainian Ye, wide CJK: 1 + 1 + 2 cells. */
    static const ns_octet_t text[] = {101,204,129,208,132,228,184,173};
    int width;
    size_t offset;
    check(text_width(NULL, (const char *)text, sizeof(text), &width) == NSERROR_OK && width == 32,
          "shared grapheme and wide-cell metrics");
    check(text_position(NULL, (const char *)text, sizeof(text), 7, &offset, &width) == NSERROR_OK &&
          offset == 3 && width == 8, "hit testing never splits a combining cluster");
    check(text_position(NULL, (const char *)text, sizeof(text), 23, &offset, &width) == NSERROR_OK &&
          offset == 5 && width == 16, "hit testing uses the midpoint of a wide character");
}
static void textplain_test(struct browser_window *bw)
{
    char path[96], address[120];
    nsurl *url = NULL;
    int fd, i;
    const int widths[] = { 480, 120, 16, 1 };
    struct rect clip = { 0, 0, 480, 8000 };
    struct redraw_context redraw = { .interactive = false, .background_images = true, .plot = &plotters };
    plain_metrics_test();
    snprintf(path, sizeof(path), "/tmp/netsurf-plain-%d.txt", (int)getpid());
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    check(fd >= 0, "create UTF-8 plain text fixture");
    if (fd < 0) return;
    check(write(fd, plain_fixture, sizeof(plain_fixture)) == sizeof(plain_fixture), "write UTF-8 fixture");
    close(fd);
    snprintf(address, sizeof(address), "file://%s", path);
    if (nsurl_create(address, &url) == NSERROR_OK) {
        check(browser_window_navigate(bw, url, NULL, BW_NAVIGATE_HISTORY, NULL, NULL, NULL) == NSERROR_OK,
              "plain text navigation");
        nsurl_unref(url);
        check(load_finished(bw), "plain text loaded");
        for (i = 0; i < 4; i++) {
            viewport = widths[i]; clip.x1 = viewport;
            browser_window_reformat(bw, false, viewport, 320);
            plain_length = 0; plain_valid = true; maximum_y = 0; capture_plain = true;
            check(browser_window_redraw(bw, 0, 0, &clip, &redraw), "plain text redraw");
            capture_plain = false;
            check(plain_valid, "plain text font size and UTF-8 line boundaries");
            check(plain_length == sizeof(plain_fixture) &&
                  memcmp(plain_drawn, plain_fixture, sizeof(plain_fixture)) == 0,
                  "default UTF-8 and lossless word wrapping at every width");
            check(maximum_y > 30, "plain text has distinct visible line baselines");
        }
    } else check(false, "plain text URL");
    check(unlink(path) == 0, "remove plain text fixture");
    plain_encoding_test(bw);
}
