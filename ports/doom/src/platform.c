/* Host adapter shared by the native reference build and the Ripple port.
 * The stock engine owns game state; this file owns time, input and presentation. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "doomtype.h"
#include "doomstat.h"
#include "doomkeys.h"
#include "d_event.h"
#include "d_main.h"
#include "i_system.h"
#include "i_video.h"
#include "i_timer.h"
#include "tables.h"
#include "w_wad.h"
#include "z_zone.h"
#include "doom_port.h"
#include "doom_resolution.h"
#if defined(RIPPLE) || defined(RCC_NATIVE)
#include <mmio.h>
#include <vm_devices.h>
#include "render_fast.h"
#endif

byte *I_VideoBuffer;
boolean screenvisible = true;
boolean screensaver_mode;
float mouse_acceleration = 1.0f;
int mouse_threshold = 10;
int vanilla_keyboard_mapping = 1;
int usegamma;
int usemouse = 1;
int screen_width = DOOM_OUTPUT_WIDTH, screen_height = DOOM_OUTPUT_HEIGHT, screen_bpp = 16;
int fullscreen, aspect_ratio_correct, show_diskicon, diskicon_readbytes;
char *video_driver = "ripple";

static unsigned short *framebuffer;
static uint16_t palette565[256];
static int palette_changed = 1;
static byte palette_rgb[768];
static uint32_t logical_tick;
static uint32_t presented;
static unsigned char held[256];
#if !defined(RIPPLE) && !defined(RCC_NATIVE)
static const char *capture_path;
static unsigned long capture_frame = 1;
static FILE *input_file;
static unsigned long input_tick;
static unsigned int input_key, input_down;
static int have_input;
static unsigned long previous_input_tick;

static void read_native_input(void)
{
    int fields = fscanf(input_file, "%lu %u %u", &input_tick, &input_key, &input_down);
    if (fields == EOF && !ferror(input_file)) {
        have_input = 0;
        return;
    }
    if (fields != 3 || !input_tick || input_tick < previous_input_tick
        || input_key >= 256 || input_down > 1)
        I_Error("Invalid native input fixture record");
    previous_input_tick = input_tick;
    have_input = 1;
}
#endif

uint32_t Doom_Tick(void) { return logical_tick; }

void Doom_NextTick(void)
{
#if defined(RIPPLE) || defined(RCC_NATIVE)
    VmClock clock;
    if (vm_next_tick(&clock) != VM_DEVICE_OK) I_Error("VM tick failed");
    if (clock.hz != 35 || clock.tick != logical_tick + 1UL)
        I_Error("Doom requires consecutive 35 Hz logical ticks");
    logical_tick = clock.tick;
#else
    ++logical_tick;
#endif
}

/* USB HID keyboard usages are stable in both the VM and the replay fixture. */
static int doom_key(unsigned int key)
{
    if (key >= 4 && key <= 29) return 'a' + key - 4;
    if (key >= 30 && key <= 38) return '1' + key - 30;
    if (key == 39) return '0';
    if (key >= 58 && key <= 67) return KEY_F1 + key - 58;
    switch (key) {
        case 40: return KEY_ENTER; case 41: return KEY_ESCAPE;
        case 42: return KEY_BACKSPACE; case 43: return KEY_TAB;
        case 44: return ' '; case 45: return '-'; case 46: return '=';
        case 54: return ','; case 55: return '.';
        case 68: return KEY_F11; case 69: return KEY_F12;
        case 72: return KEY_PAUSE;
        case 79: return KEY_RIGHTARROW; case 80: return KEY_LEFTARROW;
        case 81: return KEY_DOWNARROW; case 82: return KEY_UPARROW;
        case 224: case 228: return KEY_RCTRL;
        case 225: case 229: return KEY_RSHIFT;
        case 226: case 230: return KEY_RALT;
        default: return 0;
    }
}

void Doom_PostKey(unsigned int usage, int down)
{
    event_t event;
    int key;
    int before, after;
    if (usage >= 256 || held[usage] == (unsigned char)(down != 0)) return;
    key = doom_key(usage);
    before = held[usage];
    if (key == KEY_RCTRL) before = held[224] || held[228];
    if (key == KEY_RSHIFT) before = held[225] || held[229];
    if (key == KEY_RALT) before = held[226] || held[230];
    held[usage] = down != 0;
    after = held[usage];
    if (key == KEY_RCTRL) after = held[224] || held[228];
    if (key == KEY_RSHIFT) after = held[225] || held[229];
    if (key == KEY_RALT) after = held[226] || held[230];
    if (before == after) return;
    if (!key) return;
    memset(&event, 0, sizeof(event));
    event.type = down ? ev_keydown : ev_keyup;
    event.data1 = key;
    event.data2 = key >= 32 && key < 127 ? key : 0;
    event.data3 = event.data2;
    D_PostEvent(&event);
    /* Drain each event before posting the next, preserving same-tick ordering
     * without overflowing Doom's smaller internal event ring. */
    D_ProcessEvents();
}

/* Saturate before addition so even an extreme replay delta cannot overflow. */
long Doom_AddMouseDelta(long current, long delta)
{
    if (delta > 1024L - current) return 1024L;
    if (delta < -1024L - current) return -1024L;
    return current + delta;
}

/* Positive device Y is down; Doom uses positive Y for forward motion.
 * Bound deltas before the engine's sensitivity/fixed-point arithmetic. */
void Doom_PostMouse(unsigned int buttons, long dx, long dy)
{
    event_t event;
    if (dx > 1024L) dx = 1024L;
    if (dx < -1024L) dx = -1024L;
    if (dy > 1024L) dy = 1024L;
    if (dy < -1024L) dy = -1024L;
    memset(&event, 0, sizeof(event));
    event.type = ev_mouse;
    event.data1 = buttons & 7U;
    event.data2 = (int)dx;
    event.data3 = -(int)dy;
    D_PostEvent(&event);
    D_ProcessEvents();
}

void I_StartTic(void)
{
#if defined(RIPPLE) || defined(RCC_NATIVE)
    VmInputEvent events[16];
    VmInputBatch batch;
    VmInputState state;
    unsigned short status, n;
    unsigned int i;
    static unsigned short mouse_buttons;
    long mouse_dx = 0L, mouse_dy = 0L;
    for (;;) {
        status = vm_input_read(events, 16, &batch);
        if (status != VM_DEVICE_OK) I_Error("VM input failed");
        if (batch.overflow) {
            if (vm_input_state(&state) != VM_DEVICE_OK) I_Error("VM input state failed");
            for (i = 0; i < 256; ++i) Doom_PostKey(i, vm_key_is_down(&state, i));
            mouse_buttons = state.buttons;
            mouse_dx = mouse_dy = 0L;
            Doom_PostMouse(mouse_buttons, 0L, 0L);
            break;
        }
        for (n = 0; n < batch.count; ++n) {
            VmInputEvent *event = &events[n];
            if (event->type == VM_INPUT_KEY)
                Doom_PostKey(event->code, (event->flags & VM_INPUT_DOWN) != 0);
            else if (event->type == VM_INPUT_BUTTON) {
                mouse_buttons = event->buttons;
                Doom_PostMouse(mouse_buttons, 0L, 0L);
            } else if (event->type == VM_INPUT_MOTION && (event->flags & VM_INPUT_RELATIVE)) {
                mouse_buttons = event->buttons;
                mouse_dx = Doom_AddMouseDelta(mouse_dx, event->dx);
                mouse_dy = Doom_AddMouseDelta(mouse_dy, event->dy);
            }
        }
        if (batch.count < 16) break;
    }
    /* G_Responder replaces motion rather than accumulating it. Publish the
     * combined tick after button events so clicks cannot erase movement. */
    Doom_PostMouse(mouse_buttons, mouse_dx, mouse_dy);
#else
    while (have_input && input_tick <= logical_tick) {
        Doom_PostKey(input_key, input_down);
        read_native_input();
    }
#endif
}

void Doom_ConfigureCapture(const char *path, unsigned long frame, const char *input)
{
#if !defined(RIPPLE) && !defined(RCC_NATIVE)
    capture_path = path;
    capture_frame = frame;
    if (input) {
        input_file = fopen(input, "r");
        if (!input_file) I_Error("Cannot open native input fixture");
        read_native_input();
    }
#endif
}

void I_InitGraphics(void)
{
    I_VideoBuffer = Z_Malloc((size_t)SCREENWIDTH * SCREENHEIGHT, PU_STATIC, NULL);
    framebuffer = malloc((size_t)DOOM_OUTPUT_WIDTH * DOOM_OUTPUT_HEIGHT * sizeof(*framebuffer));
    if (!framebuffer) I_Error("Cannot allocate RGB565 framebuffer");
#if DOOM_RESOLUTION_SCALE == 1
    memset(I_VideoBuffer, 0, (size_t)SCREENWIDTH * SCREENHEIGHT);
#else
    {
        unsigned int y;
        for (y = 0; y < SCREENHEIGHT; y += DOOM_RESOLUTION_SCALE) {
            byte *row = I_VideoBuffer + (size_t)y * SCREENWIDTH;
#ifdef RIPPLE
            Doom_FillSamples(row, 0, DOOM_OUTPUT_WIDTH, DOOM_RESOLUTION_SCALE);
#else
            unsigned int x;
            for (x = 0; x < SCREENWIDTH; x += DOOM_RESOLUTION_SCALE) row[x] = 0;
#endif
        }
    }
#endif
#if defined(RIPPLE) || defined(RCC_NATIVE)
    VmDeviceCaps caps;
    if (vm_devices_caps(&caps) != VM_DEVICE_OK || (caps.bits & 15U) != 15U)
        I_Error("VM device ABI 1 with RGB565_EXT, clock and keyboard required");
    display_set_mode(DISP_MODE_RGB565_EXT);
    display_enable();
    if (vm_input_configure(VM_POINTER_RELATIVE) != VM_DEVICE_OK)
        I_Error("VM relative pointer configuration failed");
#endif
    I_SetPalette(W_CacheLumpName("PLAYPAL", PU_CACHE));
}

void I_SetPalette(byte *palette)
{
    unsigned int i;
    palette_changed = 1;
    for (i = 0; i < 256; ++i) {
        unsigned int r = gammatable[usegamma][palette[i * 3] & 255U];
        unsigned int g = gammatable[usegamma][palette[i * 3 + 1] & 255U];
        unsigned int b = gammatable[usegamma][palette[i * 3 + 2] & 255U];
        palette_rgb[i * 3] = r;
        palette_rgb[i * 3 + 1] = g;
        palette_rgb[i * 3 + 2] = b;
        /* The three fields are disjoint: addition avoids a bitwise merge on
         * Ripple without changing the RGB565 value. */
        palette565[i] = ((r >> 3) << 11) + ((g >> 2) << 5) + (b >> 3);
    }
}

int I_GetPaletteIndex(int r, int g, int b)
{
    int i, best = 0;
    int32_t distance, dr, dg, db, minimum = INT32_MAX;
    for (i = 0; i < 256; ++i) {
        dr = (int32_t)r - palette_rgb[i * 3];
        dg = (int32_t)g - palette_rgb[i * 3 + 1];
        db = (int32_t)b - palette_rgb[i * 3 + 2];
        distance = dr * dr + dg * dg + db * db;
        if (distance < minimum) { minimum = distance; best = i; }
    }
    return best;
}

static void finish_update(int repeat)
{
    size_t i;
#ifdef RIPPLE
    if (!repeat || palette_changed) {
#if DOOM_RESOLUTION_SCALE == 1
        Doom_ConvertRGB565Octets(framebuffer, I_VideoBuffer, palette565,
                                SCREENWIDTH * (unsigned int)SCREENHEIGHT);
#else
        unsigned int y;
        unsigned short *dest = framebuffer;
        const byte *source = I_VideoBuffer;
        for (y = 0; y < DOOM_OUTPUT_HEIGHT; ++y) {
            Doom_ConvertRGB565SampledRow(dest, source, palette565,
                                        DOOM_OUTPUT_WIDTH, DOOM_RESOLUTION_SCALE);
            dest += DOOM_OUTPUT_WIDTH;
            if (y + 1U < DOOM_OUTPUT_HEIGHT) source += SCREENWIDTH * DOOM_RESOLUTION_SCALE;
        }
#endif
    }
#else
    {
        unsigned int x, y;
        i = 0;
        for (y = 0; y < SCREENHEIGHT; y += DOOM_RESOLUTION_SCALE)
            for (x = 0; x < SCREENWIDTH; x += DOOM_RESOLUTION_SCALE)
                framebuffer[i++] = palette565[I_VideoBuffer[(size_t)y * SCREENWIDTH + x] & 255U];
    }
#endif
    palette_changed = 0;
    ++presented;
    printf("ENGINE frame %lu tick %lu state %d episode %d map %d\n",
           (unsigned long)presented, (unsigned long)logical_tick,
           (int)gamestate, gameepisode, gamemap);
#if defined(RIPPLE) || defined(RCC_NATIVE)
    if (vm_present_rgb565(framebuffer, DOOM_OUTPUT_WIDTH, DOOM_OUTPUT_HEIGHT, DOOM_OUTPUT_WIDTH) != VM_DEVICE_OK)
        I_Error("VM presentation failed");
#else
    if (presented == capture_frame) {
        FILE *file = fopen(capture_path, "wb");
        if (!file) I_Error("Cannot write capture");
        for (i = 0; i < (size_t)DOOM_OUTPUT_WIDTH * DOOM_OUTPUT_HEIGHT; ++i) {
            fputc(framebuffer[i] & 255U, file);
            fputc(framebuffer[i] >> 8, file);
        }
        if (fclose(file)) I_Error("Cannot finish capture");
        exit(0);
    }
#endif
}

void I_FinishUpdate(void) { finish_update(0); }
void Doom_RepeatFrame(void) { finish_update(1); }

void I_ReadScreen(byte *dest) { memcpy(dest, I_VideoBuffer, (size_t)SCREENWIDTH * SCREENHEIGHT); }
void I_ShutdownGraphics(void) {
#if defined(RIPPLE) || defined(RCC_NATIVE)
    vm_input_configure(VM_POINTER_ABSOLUTE);
#endif
}
void I_StartFrame(void) {}
void I_UpdateNoBlit(void) {}
void I_BeginRead(void) {}
void I_EndRead(void) {}
void I_SetWindowTitle(char *title) {}
void I_GraphicsCheckCommandLine(void) {}
void I_SetGrabMouseCallback(grabmouse_callback_t callback) {}
void I_EnableLoadingDisk(void) {}
void I_BindVideoVariables(void) {}
void I_DisplayFPSDots(boolean enable) {}
void I_CheckIsScreensaver(void) {}
void I_InitWindowTitle(void) {}
void I_InitWindowIcon(void) {}

int32_t I_GetTime(void) { return (int32_t)logical_tick; }
int32_t I_GetTimeMS(void)
{
    return (int32_t)((logical_tick / 35UL) * 1000UL
                  + (logical_tick % 35UL) * 1000UL / 35UL);
}
void I_Sleep(int ms) {}
void I_InitTimer(void) {}
void I_WaitVBL(int count) {}

byte *I_ZoneBase(size_t *size)
{
    byte *zone;
    *size = 8UL * 1024UL * 1024UL;
    zone = malloc(*size);
    if (!zone) I_Error("Cannot allocate Doom zone");
    return zone;
}
void I_Error(char *format, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    printf("DOOM ERROR: %s\n", buffer);
    exit(1);
}
void I_Quit(void) { exit(0); }
void I_Init(void) {}
void I_BindVariables(void) {}
void I_Tactile(int on, int off, int total) {}
void I_AtExit(atexit_func_t callback, boolean on_error) {}
boolean I_ConsoleStdout(void) { return false; }
void I_PrintBanner(char *text) { puts(text); }
void I_PrintDivider(void) {}
void I_PrintStartupBanner(char *text) { puts(text); }
boolean I_GetMemoryValue(unsigned int offset, void *value, int size)
{
    I_Error("Unsupported vanilla memory-overflow emulation");
    return false;
}
