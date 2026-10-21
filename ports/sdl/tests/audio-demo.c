/* Asset-free SDL 1.2 soundboard. Audio buffers contain logical octets on C16. */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_RATE 22050
#define VOICE_COUNT 4

typedef struct Voice {
    Uint32 phase, step, remaining, duration;
    Uint32 gain, decay;
    int kind, peak, pan;
} Voice;

static struct {
    Voice voice[VOICE_COUNT];
    unsigned int next_voice;
    int muted, sequence;
    Uint32 sequence_wait, callbacks, frames, nonzero;
} synth;

static const Sint16 sine_wave[32] = {
    0,200,392,569,724,851,946,1004,1024,1004,946,851,724,569,392,200,
    0,-200,-392,-569,-724,-851,-946,-1004,-1024,-1004,-946,-851,-724,-569,-392,-200
};
static const Uint16 melody[8] = {523,659,784,1047,784,659,587,523};
static SDL_Surface *screen;
static int audio_ready, output_channels = 2, last_sound = -1;
static const char *last_name = "READY";

/* Caller owns the SDL audio lock, or is the SDL audio callback itself. */
static void add_voice(int kind, Uint16 frequency)
{
    Voice *voice = &synth.voice[synth.next_voice++ % VOICE_COUNT];
    voice->phase = 0;
    voice->step = (Uint32)frequency * 65536UL / SAMPLE_RATE;
    voice->duration = kind == 0 ? 11025UL : kind == 1 ? 13230UL : 9922UL;
    voice->remaining = voice->duration;
    voice->kind = kind;
    voice->peak = kind == 0 ? 4400 : kind == 1 ? 5000 : 3000;
    voice->pan = kind == 0 ? 48 : kind == 1 ? 80 : 64;
    voice->gain = (Uint32)voice->peak * 65536UL;
    voice->decay = voice->gain / voice->duration;
}

static Sint32 sample_voice(Voice *voice)
{
    Sint32 wave, level;
    Uint32 age;
    int position;
    if (!voice->remaining) return 0;
    position = (int)(voice->phase >> 11);
    if (voice->kind == 0) {
        wave = (Sint32)sine_wave[position] +
            sine_wave[((voice->phase * 3UL) >> 11) & 31UL] / 3;
    } else if (voice->kind == 1) {
        position = (int)(voice->phase >> 6);
        wave = position < 512 ? -1024L + 4L * position : 3072L - 4L * position;
    } else {
        wave = voice->phase < 32768UL ? 1024 : -1024;
        if ((voice->remaining & 3UL) == 0 && voice->step > 300UL)
            --voice->step;
    }
    /* Fixed-point decay avoids a variable-width divide in every audio frame. */
    level = (Sint32)(voice->gain >> 16);
    age = voice->duration - voice->remaining;
    if (age < 220UL) level = level * (Sint32)age / 220L;
    voice->phase = (voice->phase + voice->step) & 65535UL;
    voice->gain = voice->gain > voice->decay ? voice->gain - voice->decay : 0;
    --voice->remaining;
    return wave * level / 1024L;
}

static void write_sample(Uint8 *destination, Sint32 sample)
{
    Uint16 encoded;
    if (sample > 24000L) sample = 24000L;
    if (sample < -24000L) sample = -24000L;
    encoded = (Uint16)sample;
    destination[0] = (Uint8)(encoded & 255U);
    destination[1] = (Uint8)((encoded >> 8) & 255U);
}

static void audio_callback(void *userdata, Uint8 *stream, int length)
{
    int frame, index, count = length / (output_channels * 2);
    (void)userdata;
    memset(stream, 0, (size_t)length);
    ++synth.callbacks;
    for (frame = 0; frame < count; ++frame) {
        Sint32 left = 0, right = 0;
        if (synth.sequence >= 0) {
            if (!synth.sequence_wait) {
                add_voice(0, melody[synth.sequence++]);
                synth.sequence_wait = 5512UL;
                if (synth.sequence == 8) synth.sequence = -1;
            }
            if (synth.sequence_wait) --synth.sequence_wait;
        }
        for (index = 0; index < VOICE_COUNT; ++index) {
            Voice *voice = &synth.voice[index];
            Sint32 sample;
            if (!voice->remaining) continue;
            sample = sample_voice(voice);
            left += sample * (128 - voice->pan) / 128L;
            right += sample * voice->pan / 128L;
        }
        if (synth.muted) left = right = 0;
        if (left || right) ++synth.nonzero;
        if (output_channels == 1) {
            write_sample(stream + frame * 2, left + right);
        } else {
            write_sample(stream + frame * 4, left);
            write_sample(stream + frame * 4 + 2, right);
        }
    }
    synth.frames += (Uint32)count;
}

static int open_audio(void)
{
    SDL_AudioSpec wanted, obtained;
    memset(&wanted, 0, sizeof(wanted));
    wanted.freq = SAMPLE_RATE;
    wanted.format = AUDIO_S16LSB;
    wanted.channels = (Uint8)output_channels;
    wanted.samples = 512;
    wanted.callback = audio_callback;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0 || SDL_OpenAudio(&wanted, &obtained) < 0) {
        fprintf(stderr, "SDL_AUDIO_DEMO audio unavailable: %s\n", SDL_GetError());
        return 0;
    }
    if (obtained.freq != wanted.freq || obtained.format != wanted.format ||
        obtained.channels != wanted.channels) {
        fprintf(stderr, "SDL_AUDIO_DEMO audio format unavailable\n");
        SDL_CloseAudio();
        return 0;
    }
    audio_ready = 1;
    SDL_PauseAudio(0);
    return 1;
}

static void trigger(int sound)
{
    static const Uint16 notes[3] = {659,220,1000};
    if (!audio_ready) return;
    SDL_LockAudio();
    if (sound == 3) {
        synth.sequence = 0;
        synth.sequence_wait = 0;
    } else {
        add_voice(sound, notes[sound]);
    }
    SDL_UnlockAudio();
    last_sound = sound;
    last_name = sound == 0 ? "CHIME" : sound == 1 ? "PLUCK" : sound == 2 ? "ZAP" : "MELODY";
}

static void stop_sounds(void)
{
    if (!audio_ready) return;
    SDL_LockAudio();
    memset(synth.voice, 0, sizeof(synth.voice));
    synth.sequence = -1;
    synth.sequence_wait = 0;
    SDL_UnlockAudio();
    last_sound = -1;
    last_name = "STOPPED";
}

/* Five by seven bitmap letters, each row stored in its low five bits. */
static const Uint8 font[36][7] = {
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
    {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
    {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
    {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
    {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
    {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
    {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
    {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
    {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
    {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14}
};

static void fill(int x, int y, int width, int height, int r, int g, int b)
{
    SDL_Rect rect;
    rect.x = x; rect.y = y; rect.w = width; rect.h = height;
    if (SDL_FillRect(screen, &rect, SDL_MapRGB(screen->format, r, g, b)) < 0) {
        fprintf(stderr, "SDL_AUDIO_DEMO drawing failed: %s\n", SDL_GetError());
        SDL_Quit(); exit(1);
    }
}

static void text(int x, int y, const char *label, int scale, int r, int g, int b)
{
    int row, column, glyph;
    while (*label) {
        glyph = *label >= 'A' && *label <= 'Z' ? *label - 'A' :
            *label >= '0' && *label <= '9' ? *label - '0' + 26 : -1;
        if (glyph >= 0) {
            for (row = 0; row < 7; ++row)
                for (column = 0; column < 5; ++column)
                    if (font[glyph][row] & (16U >> column))
                        fill(x + column * scale, y + row * scale, scale, scale, r, g, b);
        } else if (*label == '-') {
            fill(x, y + 3 * scale, 5 * scale, scale, r, g, b);
        }
        x += 6 * scale;
        ++label;
    }
}

static void draw(void)
{
    static const int colors[3][3] = {{97,197,238},{162,220,145},{245,179,105}};
    static const char *names[3] = {"CHIME","PLUCK","ZAP"};
    static const char *details[3] = {"BELL TONE","TRIANGLE","ARCADE"};
    int index, x, point, high, muted;
    muted = 0;
    if (audio_ready) {
        SDL_LockAudio(); muted = synth.muted; SDL_UnlockAudio();
    }
    fill(0,0,640,400,20,25,34);
    text(24,24,"SDL SOUND LAB",3,237,242,248);
    text(25,65,output_channels == 1 ? "22050 HZ - MONO - 16 BIT" :
         "22050 HZ - STEREO - 16 BIT",2,142,158,181);
    text(25,100,"CLICK A PAD OR PRESS 1 2 3",2,200,211,225);
    for (index = 0; index < 3; ++index) {
        char number[2];
        x = 24 + index * 200;
        fill(x,137,192,142,36,45,59);
        fill(x,137,192,last_sound == index ? 5 : 2,
             colors[index][0],colors[index][1],colors[index][2]);
        number[0] = (char)('1' + index); number[1] = 0;
        text(x+14,153,number,3,colors[index][0],colors[index][1],colors[index][2]);
        for (point = 0; point < 32; ++point) {
            if (index == 0) high = sine_wave[point] / 128;
            else if (index == 1) high = point < 16 ? point - 8 : 24 - point;
            else high = (point & 8) ? 7 : -7;
            fill(x+52+point*4,169-high,3,3,
                 colors[index][0],colors[index][1],colors[index][2]);
        }
        text(x+14,204,names[index],3,237,242,248);
        text(x+14,248,details[index],1,160,178,199);
    }
    if (!audio_ready) text(25,303,"AUDIO UNAVAILABLE - SEE TERMINAL",2,245,179,105);
    else if (muted) text(25,303,"MUTED - PRESS M TO HEAR SOUND",2,245,179,105);
    else {
        text(25,303,"LAST SOUND",2,142,158,181);
        text(169,303,last_name,2,200,222,237);
    }
    fill(24,338,592,1,61,75,94);
    text(25,357,"SPACE MELODY   S STOP   M MUTE   ESC QUIT",2,180,195,214);
    SDL_Flip(screen);
}

static int self_test(void)
{
    Uint32 callbacks, frames, nonzero;
    if (SDL_Init(0) < 0 || !open_audio()) { SDL_Quit(); return 1; }
    trigger(0);
    SDL_Delay(700);
    SDL_PauseAudio(1);
    SDL_LockAudio();
    callbacks = synth.callbacks; frames = synth.frames; nonzero = synth.nonzero;
    SDL_UnlockAudio();
    SDL_CloseAudio();
    SDL_Quit();
    printf("SDL_AUDIO_DEMO self-test callbacks=%lu frames=%lu nonzero=%lu\n",
           (unsigned long)callbacks,(unsigned long)frames,(unsigned long)nonzero);
    return callbacks >= 2UL && nonzero ? 0 : 1;
}

int main(int argc, char **argv)
{
    int index, testing = 0, quit = 0;
    synth.sequence = -1;
    for (index = 1; index < argc; ++index) {
        if (!strcmp(argv[index],"--self-test")) testing = 1;
        else if (!strcmp(argv[index],"--mono")) output_channels = 1;
        else {
            fprintf(stderr,"usage: sdl-audio-demo [--mono] [--self-test]\n");
            return 2;
        }
    }
    if (testing) return self_test();
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr,"SDL_AUDIO_DEMO video init failed: %s\n",SDL_GetError());
        return 1;
    }
    SDL_WM_SetCaption("SDL Sound Lab",NULL);
    screen = SDL_SetVideoMode(640,400,32,SDL_SWSURFACE);
    if (!screen) {
        fprintf(stderr,"SDL_AUDIO_DEMO video mode failed: %s\n",SDL_GetError());
        SDL_Quit(); return 1;
    }
    open_audio();
    draw();
    while (!quit) {
        SDL_Event event;
        int dirty = 0;
        /* The audio backend must keep playing while this application is idle. */
        if (!SDL_WaitEvent(&event)) {
            fprintf(stderr,"SDL_AUDIO_DEMO wait failed: %s\n",SDL_GetError());
            SDL_Quit(); return 1;
        }
        switch (event.type) {
        case SDL_QUIT: quit = 1; break;
        case SDL_VIDEOEXPOSE: dirty = 1; break;
        case SDL_MOUSEBUTTONDOWN:
            if (event.button.button == SDL_BUTTON_LEFT &&
                event.button.y >= 137 && event.button.y < 279) {
                for (index = 0; index < 3; ++index)
                    if (event.button.x >= 24 + index * 200 &&
                        event.button.x < 216 + index * 200) {
                        trigger(index); dirty = 1; break;
                    }
            }
            break;
        case SDL_KEYDOWN:
            if (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q)
                quit = 1;
            else if (event.key.keysym.sym >= SDLK_1 && event.key.keysym.sym <= SDLK_3) {
                trigger(event.key.keysym.sym - SDLK_1); dirty = 1;
            } else if (event.key.keysym.sym == SDLK_SPACE) {
                trigger(3); dirty = 1;
            } else if (event.key.keysym.sym == SDLK_s) {
                stop_sounds(); dirty = 1;
            } else if (event.key.keysym.sym == SDLK_m && audio_ready) {
                SDL_LockAudio(); synth.muted = !synth.muted; SDL_UnlockAudio();
                dirty = 1;
            }
            break;
        default: break;
        }
        if (dirty && !quit) draw();
    }
    if (audio_ready) { SDL_PauseAudio(1); SDL_CloseAudio(); }
    SDL_Quit();
    printf("SDL_AUDIO_DEMO quit\n");
    return 0;
}
