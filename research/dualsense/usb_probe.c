/* macOS DualSense workbench. Deliberately no arbitrary command or loader API.
 * Query provenance: pinned calibration-ds5-controller.js in analysis/followup/reference/.
 * clang -Wall -Wextra -Werror usb_probe.c -framework IOKit -framework CoreFoundation -o usb_probe
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <mach/mach_time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "usb_policy.h"

static IOHIDDeviceRef pad;
static uint8_t input[64], firmware[64], calibration[64];
static CFIndex firmware_len, calibration_len;
static unsigned input_count, input_bad;
static bool input_changed;
static uint8_t previous_input[64];
static CFIndex previous_input_len;
static uint64_t started;
static mach_timebase_info_data_t timebase;
static bool fatal;

static double ms(void) {
    return (double)(mach_absolute_time() - started) * timebase.numer / timebase.denom / 1e6;
}

static void hex(const uint8_t *data, size_t count) {
    for (size_t i = 0; i < count; ++i) printf("%02x", data[i]);
}

static void event(const char *name) {
    printf("{\"event\":\"%s\",\"ms\":%.3f}\n", name, ms());
}

static bool get_report(uint8_t id, CFIndex capacity, uint8_t *out, CFIndex *actual) {
    if (capacity < 1 || !ds_get_allowed(id, (size_t)capacity)) {
        fprintf(stderr, "Read rejected by the fixed probe policy\n"); fatal = true; return false;
    }
    struct { uint8_t before[16], data[64], after[16]; } guarded;
    memset(&guarded, 0xa5, sizeof(guarded));
    guarded.data[0] = id;
    CFIndex length = capacity;
    printf("{\"event\":\"get_begin\",\"id\":%u,\"capacity\":%ld,\"ms\":%.3f}\n", id, capacity, ms());
    double begin = ms();
    IOReturn rc = IOHIDDeviceGetReport(pad, kIOHIDReportTypeFeature, id, guarded.data, &length);
    bool guard_ok = true;
    for (size_t i = 0; i < 16; ++i)
        guard_ok &= guarded.before[i] == 0xa5 && guarded.after[i] == 0xa5;
    bool bounded = length >= 0 && length <= capacity;
    /* Check host API writes beyond the requested buffer, not target RAM safety. */
    for (CFIndex i = capacity; i < 64; ++i) guard_ok &= guarded.data[i] == 0xa5;
    printf("{\"event\":\"get_end\",\"id\":%u,\"capacity\":%ld,\"actual\":%ld,\"rc\":%u,\"elapsed_ms\":%.3f,\"guard_ok\":%s,\"hex\":\"",
           id, capacity, length, (uint32_t)rc, ms() - begin, guard_ok ? "true" : "false");
    if (rc == kIOReturnSuccess && bounded) hex(guarded.data, (size_t)length);
    printf("\"}\n");
    if (!guard_ok || !bounded) {
        fprintf(stderr, "HID returned an invalid length or modified host buffer guards\n");
        fatal = true;
        return false;
    }
    if (rc != kIOReturnSuccess) return false;
    memcpy(out, guarded.data, (size_t)length);
    *actual = length;
    return true;
}

static bool send_report(IOHIDReportType type, const uint8_t *bytes, CFIndex count) {
    if (count < 1 || !ds_send_allowed(type, bytes, (size_t)count)) {
        fprintf(stderr, "Write rejected by the fixed probe policy\n"); fatal = true; return false;
    }
    printf("{\"event\":\"set_begin\",\"type\":%d,\"id\":%u,\"ms\":%.3f,\"hex\":\"", type, bytes[0], ms());
    hex(bytes, (size_t)count);
    printf("\"}\n");
    double begin = ms();
    IOReturn rc = IOHIDDeviceSetReport(pad, type, bytes[0], bytes, count);
    printf("{\"event\":\"set_end\",\"rc\":%u,\"elapsed_ms\":%.3f}\n", (uint32_t)rc, ms() - begin);
    return rc == kIOReturnSuccess;
}

static void on_input(void *context, IOReturn result, void *sender,
                     IOHIDReportType type, uint32_t report_id, uint8_t *report, CFIndex count) {
    (void)context; (void)sender;
    if (result != kIOReturnSuccess || type != kIOHIDReportTypeInput ||
        report_id != 1 || count != 64) { ++input_bad; return; }
    ++input_count;
    if (previous_input_len == count && memcmp(report, previous_input, (size_t)count)) input_changed = true;
    memcpy(previous_input, report, (size_t)count);
    previous_input_len = count;
}

static void pump(double seconds) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
}

static bool health(void) {
    uint8_t data[64]; CFIndex count = 0;
    bool ok = get_report(0x20, 64, data, &count) && count == firmware_len &&
              memcmp(data, firmware, (size_t)count) == 0;
    printf("{\"event\":\"health\",\"ok\":%s,\"ms\":%.3f}\n", ok ? "true" : "false", ms());
    return ok;
}

static bool queries(void) {
    /* Only published query operations. Never NVS lock/unlock, calibration,
     * reset, factory audio tests, pairing, firmware update or loader entry. */
    const uint8_t commands[][2] = {{3,3}, {12,2}, {5,2}, {5,4}};
    for (size_t i = 0; i < sizeof(commands)/sizeof(commands[0]); ++i) {
        for (unsigned repetition = 0; repetition < 3; ++repetition) {
            uint8_t request[64] = {0x80}, response[64]; CFIndex count = 0;
            request[1] = commands[i][0]; request[2] = commands[i][1];
            if (!send_report(kIOHIDReportTypeFeature, request, sizeof(request))) return false;
            pump(0.1);
            if (!get_report(0x81, 64, response, &count)) return false;
            bool echo = count >= 4 && response[0] == 0x81 && response[1] == request[1] && response[2] == request[2];
            printf("{\"event\":\"query\",\"group\":%u,\"command\":%u,\"repetition\":%u,\"echo\":%s,\"status\":%d}\n",
                   request[1], request[2], repetition, echo ? "true" : "false", count >= 4 ? response[3] : -1);
            if (!echo) {
                fprintf(stderr, "Unexpected factory reply; stopping instead of guessing another command\n");
                return false;
            }
            if (!health()) return false;
        }
    }
    return true;
}

static bool lengths(void) {
    const CFIndex sizes[] = {1,2,4,8,16,32,63,64};
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); ++i) {
        uint8_t data[64]; CFIndex count = 0;
        bool got = get_report(0x20, sizes[i], data, &count);
        bool prefix = got && count <= firmware_len && !memcmp(data, firmware, (size_t)count);
        printf("{\"event\":\"length_probe\",\"capacity\":%ld,\"received\":%s,\"matches_prefix\":%s}\n",
               sizes[i], got ? "true" : "false", prefix ? "true" : "false");
        /* A rejected short read is allowed, an unexpected successful reply isn't. */
        if (fatal || (got && !prefix) || !health()) return false;
    }
    return true;
}

static bool query_shapes(void) {
    /* Change framing of known queries, never command IDs or payload values.
     * Baseline each reply, then compare short requests and short/repeated reads.
     * These are host API sizes; without a USB capture they aren't proof of
     * the control transfer's exact on-wire wLength. */
    const uint8_t commands[][2] = {{3,3}, {12,2}, {5,2}, {5,4}, {3,3}};
    const CFIndex request_sizes[] = {3,4,8,16,64};
    const CFIndex read_sizes[] = {4,8,16,28,63,64};
    for (size_t i = 0; i < sizeof(commands)/sizeof(commands[0]); ++i) {
        uint8_t request[64] = {0x80}, expected[64]; CFIndex expected_count = 0;
        request[1] = commands[i][0]; request[2] = commands[i][1];
        if (!send_report(kIOHIDReportTypeFeature, request, sizeof(request))) return false;
        pump(0.1);
        if (!get_report(0x81, 64, expected, &expected_count) || expected_count != 64 ||
            expected[0] != 0x81 || expected[1] != request[1] || expected[2] != request[2] || expected[3] != 2) return false;
        size_t request_cases = sizeof(request_sizes)/sizeof(request_sizes[0]);
        size_t read_cases = sizeof(read_sizes)/sizeof(read_sizes[0]);
        for (size_t j = 0; j < request_cases + read_cases; ++j) {
            CFIndex request_size = j < request_cases ? request_sizes[j] : 64;
            CFIndex read_size = j < request_cases ? 64 : read_sizes[j - request_cases];
            bool sent = send_report(kIOHIDReportTypeFeature, request, request_size);
            printf("{\"event\":\"query_shape\",\"group\":%u,\"command\":%u,\"request_size\":%ld,\"read_size\":%ld,\"sent\":%s}\n",
                   request[1], request[2], request_size, read_size, sent ? "true" : "false");
            if (fatal) return false;
            if (!sent) { if (!health()) return false; continue; }
            pump(0.1);
            uint8_t response[64]; CFIndex count = 0;
            bool got = get_report(0x81, read_size, response, &count);
            bool prefix = got && count == read_size && !memcmp(response, expected, (size_t)count);
            printf("{\"event\":\"query_shape_reply\",\"capacity\":%ld,\"received\":%s,\"matches_prefix\":%s}\n",
                   read_size, got ? "true" : "false", prefix ? "true" : "false");
            if (fatal || !prefix) return false;
            /* First live framing test revealed one-shot replies. Repeated GET
             * reads an idle result, not another piece of the previous reply. */
            if (!get_report(0x81, 64, response, &count)) return false;
            bool idle = count == 64 && response[0] == 0x81;
            for (CFIndex k = 1; k < count; ++k) idle &= response[k] == 0;
            printf("{\"event\":\"mailbox_idle\",\"ok\":%s}\n", idle ? "true" : "false");
            if (!idle) return false;
            if (!health()) return false;
        }
    }
    return true;
}

static bool controls(void) {
    /* Normal output report 0x02; no adaptive-trigger or audio configuration. */
    uint8_t report[48] = {0x02};
    report[2] = 0x04; report[45] = 16; report[47] = 16;
    bool ok = send_report(kIOHIDReportTypeOutput, report, sizeof(report));
    pump(0.25);
    report[45] = 0; report[47] = 0;
    bool stopped = send_report(kIOHIDReportTypeOutput, report, sizeof(report));
    if (!ok || !stopped || !health()) return false;
    memset(report, 0, sizeof(report)); report[0] = 0x02;
    report[1] = 0x03; report[3] = 24; report[4] = 24;
    ok = send_report(kIOHIDReportTypeOutput, report, sizeof(report));
    pump(0.15);
    report[3] = 0; report[4] = 0;
    stopped = send_report(kIOHIDReportTypeOutput, report, sizeof(report));
    return ok && stopped && health();
}

static bool extra_reports(void) {
    /* Unknown semantics: bounded GET only, never SET, at declared size + ID.
     * Treat a rejected GET as an explicit observation, then check health. */
    const uint8_t ids[] = {0x0b,0x0c};
    for (size_t i = 0; i < sizeof(ids); ++i) {
        uint8_t previous[64]; CFIndex previous_count = 0;
        for (unsigned j = 0; j < 3; ++j) {
            uint8_t response[64]; CFIndex count = 0;
            bool got = get_report(ids[i], 42, response, &count);
            printf("{\"event\":\"extra_report\",\"id\":%u,\"repetition\":%u,\"received\":%s,\"same_as_previous\":%s}\n",
                   ids[i], j, got ? "true" : "false",
                   got && previous_count > 0 && count == previous_count && !memcmp(response, previous, (size_t)count) ? "true" : "false");
            if (fatal || (got && (count < 1 || response[0] != ids[i])) || !health()) return false;
            if (!got) break;
            memcpy(previous, response, (size_t)count); previous_count = count;
            pump(0.05);
        }
    }
    return true;
}

static bool telemetry(void) {
    /* Sony GetTelemetryDataDualSense -> RVA 0x3400 in hash-pinned DLL.
     * Six sequential 64-byte replies carry 56 data bytes each (offset 4).
     * No address, offset or variable payload is supplied to the controller. */
    unsigned update = firmware[44] | ((unsigned)firmware[45] << 8);
    if (firmware[20] != 3 || firmware[21] != 0 || update <= 0x452) {
        fprintf(stderr, "Telemetry gate: this probe requires standard controller firmware newer than 0x0452\n");
        return false;
    }
    const uint8_t request[64] = {0x80,0x70,1};
    if (!send_report(kIOHIDReportTypeFeature, request, sizeof(request))) return false;
    pump(0.1);
    for (unsigned i = 0; i < 6; ++i) {
        uint8_t response[64]; CFIndex count = 0;
        if (!get_report(0x81, 64, response, &count)) return false;
        bool valid = count == 64 && response[0] == 0x81 && response[1] == 0x70 && response[2] == 1 &&
                     response[3] == (i == 5 ? 2 : 3);
        printf("{\"event\":\"telemetry_chunk\",\"index\":%u,\"valid\":%s,\"status\":%d}\n",
               i, valid ? "true" : "false", count >= 4 ? response[3] : -1);
        if (!valid) {
            fprintf(stderr, "Telemetry reply differed from the traced framing; stopping\n");
            return false;
        }
    }
    return health();
}

static bool identity(void) {
    /* Exact published getSystemInfo calls: MCU unique ID and PCBA ID.
     * No selector sweep, user-specified subcommand or persistent operation. */
    const uint8_t commands[] = {9,17};
    for (size_t i = 0; i < sizeof(commands); ++i) {
        const uint8_t request[64] = {0x80,1,commands[i]};
        uint8_t response[64]; CFIndex count = 0;
        if (!send_report(kIOHIDReportTypeFeature, request, sizeof(request))) return false;
        pump(0.1);
        if (!get_report(0x81, 64, response, &count) || count != 64 || response[0] != 0x81 ||
            response[1] != 1 || response[2] != commands[i] || response[3] != 2) return false;
        if (!health()) return false;
    }
    return true;
}

static void property(IOHIDDeviceRef device, CFStringRef key, const char *name) {
    CFTypeRef value = IOHIDDeviceGetProperty(device, key);
    char text[256] = {0}; long number = 0;
    if (value && CFGetTypeID(value) == CFStringGetTypeID() &&
        CFStringGetCString(value, text, sizeof(text), kCFStringEncodingUTF8))
        printf("{\"event\":\"property\",\"name\":\"%s\",\"value\":\"%s\"}\n", name, text);
    else if (value && CFGetTypeID(value) == CFNumberGetTypeID() && CFNumberGetValue(value, kCFNumberLongType, &number))
        printf("{\"event\":\"property\",\"name\":\"%s\",\"value\":%ld}\n", name, number);
}

int main(int argc, char **argv) {
    bool do_queries = false, do_lengths = false, do_controls = false, do_shapes = false;
    bool do_extra = false, do_telemetry = false, do_identity = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--queries")) do_queries = true;
        else if (!strcmp(argv[i], "--lengths")) do_lengths = true;
        else if (!strcmp(argv[i], "--controls")) do_controls = true;
        else if (!strcmp(argv[i], "--query-shapes")) do_shapes = true;
        else if (!strcmp(argv[i], "--extra-reports")) do_extra = true;
        else if (!strcmp(argv[i], "--telemetry")) do_telemetry = true;
        else if (!strcmp(argv[i], "--identity")) do_identity = true;
        else { fprintf(stderr, "Usage: usb_probe [--queries] [--lengths] [--controls] [--query-shapes] [--extra-reports] [--telemetry] [--identity]\n"); return 2; }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    mach_timebase_info(&timebase); started = mach_absolute_time();
    int vendor = 0x054c, product = 0x0ce6;
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberIntType, &vendor);
    CFNumberRef p = CFNumberCreate(NULL, kCFNumberIntType, &product);
    const void *keys[] = {CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey), CFSTR(kIOHIDTransportKey)};
    const void *values[] = {v,p,CFSTR("USB")};
    CFDictionaryRef match = CFDictionaryCreate(NULL, keys, values, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    IOHIDManagerRef manager = IOHIDManagerCreate(NULL, kIOHIDOptionsTypeNone);
    IOHIDManagerSetDeviceMatching(manager, match);
    CFRelease(match); CFRelease(v); CFRelease(p);
    CFSetRef devices = IOHIDManagerCopyDevices(manager);
    if (!devices || CFSetGetCount(devices) != 1) {
        fprintf(stderr, "Expected exactly one USB 054c:0ce6 controller; found %ld\n", devices ? CFSetGetCount(devices) : 0);
        if (devices) CFRelease(devices);
        CFRelease(manager); return 1;
    }
    const void *selected; CFSetGetValues(devices, &selected); pad = (IOHIDDeviceRef)selected;
    IOReturn rc = IOHIDDeviceOpen(pad, kIOHIDOptionsTypeNone);
    if (rc != kIOReturnSuccess) { fprintf(stderr, "HID open failed: 0x%08x\n", (unsigned)rc); CFRelease(devices); CFRelease(manager); return 1; }
    event("opened");
    property(pad, CFSTR(kIOHIDProductKey), "product");
    property(pad, CFSTR(kIOHIDTransportKey), "transport");
    property(pad, CFSTR(kIOHIDMaxInputReportSizeKey), "max_input");
    property(pad, CFSTR(kIOHIDMaxOutputReportSizeKey), "max_output");
    property(pad, CFSTR(kIOHIDMaxFeatureReportSizeKey), "max_feature");
    CFTypeRef descriptor = IOHIDDeviceGetProperty(pad, CFSTR(kIOHIDReportDescriptorKey));
    if (!descriptor || CFGetTypeID(descriptor) != CFDataGetTypeID()) {
        fprintf(stderr, "Missing report descriptor\n"); IOHIDDeviceClose(pad, 0); CFRelease(devices); CFRelease(manager); return 1;
    }
    printf("{\"event\":\"descriptor\",\"hex\":\"");
    hex(CFDataGetBytePtr(descriptor), (size_t)CFDataGetLength(descriptor)); printf("\"}\n");
    IOHIDDeviceRegisterInputReportCallback(pad, input, sizeof(input), on_input, NULL);
    IOHIDDeviceScheduleWithRunLoop(pad, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    pump(0.25);
    bool ok = get_report(0x20, 64, firmware, &firmware_len) && firmware_len == 64 && firmware[0] == 0x20;
    ok = ok && get_report(0x05, 64, calibration, &calibration_len) && calibration_len == 41 && calibration[0] == 5;
    if (ok && do_queries) ok = queries();
    if (ok && do_lengths) ok = lengths();
    if (ok && do_shapes) ok = query_shapes();
    if (ok && do_controls) ok = controls();
    if (ok && do_extra) ok = extra_reports();
    if (ok && do_telemetry) ok = telemetry();
    if (ok && do_identity) ok = identity();
    if (ok) {
        uint8_t data[64]; CFIndex count = 0;
        ok = get_report(5, 64, data, &count) && count == calibration_len && !memcmp(data, calibration, (size_t)count) && health();
        printf("{\"event\":\"calibration_unchanged\",\"ok\":%s}\n", ok ? "true" : "false");
    }
    pump(0.25);
    ok = ok && input_count > 0 && input_bad == 0 && !fatal;
    printf("{\"event\":\"input_summary\",\"count\":%u,\"invalid\":%u,\"changed\":%s}\n", input_count, input_bad, input_changed ? "true" : "false");
    IOHIDDeviceUnscheduleFromRunLoop(pad, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    rc = IOHIDDeviceClose(pad, kIOHIDOptionsTypeNone);
    ok = ok && rc == kIOReturnSuccess;
    CFRelease(devices); CFRelease(manager);
    printf("{\"event\":\"complete\",\"ok\":%s,\"ms\":%.3f}\n", ok ? "true" : "false", ms());
    return ok ? 0 : 1;
}
