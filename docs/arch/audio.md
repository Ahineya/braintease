# PCM audio and OPL3 music

PCM uses the existing DEVICE_* mailbox at 27–31. MMIO 10–15 remain reserved.
Device ABI remains 1. RVM and the RCC native runtime advertise `VM_CAP_PCM`
(0x0080) and `VM_CAP_MUSIC` (0x0100); RBF omits both and returns device status 7 (unsupported) for audio
commands without reading or writing a descriptor. Discovery does not open an
operating-system sound device.

Samples are signed 16-bit two's-complement integers, one sample per guest cell.
Mono frames contain one sample; stereo frames interleave left and right.
Descriptor lengths, pointers and strides count cells. Multiword integers are
stored low 16-bit word first. Include `<vm_audio.h>` and link the runtime.

## Commands

All records start with `[1, exact_length]`. Invalid descriptors and RAM ranges
are rejected before queue changes or output writes. Descriptors and sample
buffers may cross banks; they must occupy ordinary RAM and must not overlap.

| Command | ID | Cells | Fields after header |
|---|---|---|---|
| CONFIGURE | 0x0200 | 8 | rate u32, channels u16, queue capacity in frames u32, reserved=0 |
| SUBMIT | 0x0201 | 10 | buffer bank, offset, frame count u32, flags=0, accepted count u32 (out), reserved=0 |
| STATE | 0x0202 | 16 | flags, channels, rate u32, capacity u32, queued frames u32, rendered position u64, missing frames u32 |
| CONTROL | 0x0203 | 4 | action, value |
| RENDER | 0x0204 | 6 | frames u32, reserved=0, reserved=0 |

Rates are 8,000–96,000 Hz, with one or two channels. Queue capacity is 1–192,000
frames and at most two seconds at the configured rate. CONFIGURE starts paused
and clears the logical position, fractional tick remainder, underruns and gain.
It requires an empty, stopped/paused queue. SUBMIT copies all samples immediately:
callers can overwrite or free their buffer on success. When the queue has
insufficient space, status 8 (would block) leaves it and the descriptor unchanged.
A submission cannot exceed the configured capacity.

STATE flags: configured=1, running=2, underrun seen=4, output error=8. Missing
frames saturate at UINT32_MAX. Reading STATE does not clear counters or advance
playback. The position counts logical rendered frames, not hardware callback
progress.

CONTROL actions:

| Action | Value | Effect |
|---|---|---|
| START=1 | 0 | Run/resume |
| PAUSE=2 | 0 | Pause, retaining guest samples and fractional timing |
| STOP=3 | 0 | Pause, discard queued samples, clear fractional timing |
| RESET=4 | 0 | Unconfigure and reset logical state |
| GAIN=5 | 0–65535 | Master gain; 65535 is unity |

Pause/stop/reset clear pending speaker output too. Logical position and missing
frame counts survive STOP and PAUSE; RESET/CONFIGURE clear them. Gain multiplies
samples in signed 32-bit arithmetic and divides by 65535, truncating toward zero.
New errors are 7 unsupported, 8 would block, 9 invalid playback state, and 10 host
capture I/O failure. An I/O failure can occur after logical samples were consumed;
it latches an output error and the runners report it rather than claiming a
successful capture.

## Time and output

A running device renders `rate / tick_hz` frames on each completed NEXT_TICK,
carrying the integer remainder so non-divisible rates do not drift. Queue empty
space renders as silence and increments the missing-frame counter. Pause freezes
the logical audio position. Presenting, submitting samples and reading status
never advance it. Programs without NEXT_TICK can use RENDER to advance an exact
number of frames (1–192,000). Explicit renders and tick advances are additive.

The speaker is an optional sink for these samples. CPAL uses the host's default
output device, with linear rate conversion and a bounded quarter-second host
queue. A late guest can cause physical underruns; rendering ahead can discard
old speaker frames to bound latency. Those hardware scheduling effects never
change guest state or deterministic captures. Missing hardware produces a
warning and muted live output; the logical device and WAV capture still work.
On Linux, building CPAL's default backend requires ALSA development libraries.

RVM defaults to speaker output for paced, non-headless runs.
`--audio off` mutes speakers while preserving PCM processing. `--audio-wav FILE`
writes a standard signed 16-bit WAV, including in headless/unthrottled/stepped
runs. Headers are updated incrementally, so stopped runs retain valid captures.
One capture has one rate/channel format; changing it after writing samples is
an explicit error. RESET can start another logical segment of the same format.

The RCC native runtime uses `RCC_NATIVE_AUDIO=off` and
`RCC_NATIVE_AUDIO_WAV=FILE`; its existing headless/capture and unthrottled modes
suppress automatic speaker playback. Console programs which never configure
PCM never initialize audio hardware. Checkpoints serialize the bounded guest
queue, logical cursor, remainder, gain and counters; speaker/file handles are
not copied into checkpoints.

## OPL3 music

The music device sequences MUS scores at 140 ticks per second, using the supplied
GENMIDI bank to program [Nuked OPL3](https://github.com/tgies/nuked-opl3-rs)
(`nuked-opl3` 0.1.0, LGPL-2.1-or-later). It mixes the resulting stereo samples
with PCM, saturating the sum to signed 16-bit before master gain. No external
soundfont, MIDI service, or host audio clock is involved. This first interface accepts MUS/GENMIDI;
standard MIDI files and direct OPL register access are outside its scope.

Include `<vm_music.h>`. Configure the PCM transport first, then load a bank and
score, issue MUSIC_PLAY, and start/resume PCM. NEXT_TICK and RENDER advance both
sources. A music-only application can leave the PCM sample queue empty.

| Command | ID | Cells | Fields after header |
|---|---|---|---|
| MUSIC_BANK | 0x0210 | 8 | buffer bank, offset, octet count u32, reserved=0, reserved=0 |
| MUSIC_SCORE | 0x0211 | 8 | buffer bank, offset, octet count u32, reserved=0, reserved=0 |
| MUSIC_CONTROL | 0x0212 | 4 | action, value |
| MUSIC_STATE | 0x0213 | 10 | flags, volume, score position in frames u64, reserved=0 (out), reserved=0 (out) |

Buffers hold one octet per cell (0–255), are copied immediately, can cross banks,
and must not overlap the descriptor or MMIO. MUSIC_BANK takes the first 6308
bytes of a GENMIDI lump: its header and 175 instrument records, excluding names.
MUSIC_SCORE accepts up to 131072 octets, 65536 events, and 30 minutes at 140 Hz.
Headers, ranges, event operands, and bounded variable-length delays are checked
before replacing the previous score or bank. Zero-duration scores are rejected.
Both load commands stop music, retaining its volume. PCM CONFIGURE/RESET clears
music, its bank and score; PCM STOP/PAUSE freezes the shared transport.

Music controls: PLAY=1 takes 0 (once) or 1 (loop); STOP=2, PAUSE=3 and RESUME=4
take zero; VOLUME=5 takes 0–127. Music pause retains notes and the score position
while PCM effects continue. Volume zero mutes music while its score advances.
Stop/end reset its score position; a loop restarts the score and synthesizer at
the first frame after the score duration (rounded up to a complete sample).
Music changes take effect at the next logical render, independently of queued
PCM effects. MUSIC_STATE flags are bank loaded=1, score loaded=2, playing=4,
paused=8, looping requested=16. Position excludes paused time and wraps on loops.
The shared PCM state reports the total transport position.

The renderer supports 18 two-operator voices, layered/fixed-pitch GENMIDI
instruments, percussion, pitch bend, channel volume, expression, stereo pan,
sustain, all-notes/all-sounds-off and controller reset. Unsupported effects such
as reverb/chorus are ignored. Voice exhaustion replaces the oldest voice. This
recreates OPL timbres, without promising bit-exact reproduction of DMX's voice
allocation or volume curve.

Checkpoints save the bounded bank/score, playback position, volume and controls.
Restoration deterministically replays synthesis up to that position before the
next render; this can take time for long tracks. No sound is replayed to the
speaker or capture during reconstruction, and looping bounds the replay length.

## Doom

Normal Ripple and RCC native Doom builds enable sound effects. Music is disabled
by default because of its current performance cost. The sound effects module
lazily caches DMX format-3 lumps, validates lengths and rates, strips the 16 guard
samples at each end, converts unsigned 8-bit samples to signed PCM, resamples to
22,050 Hz, mixes up to 16 voices, applies volume/panning, and clips stereo output.
It queues 630-frame blocks before the next 35 Hz tick grant, including boot
ticks which precede the first visible frame. Idle blocks avoid per-voice
mixing loops. Optional music uses the host OPL3 device described above.

```sh
ports/doom/run.sh --scene e1m1 --jit
ports/doom/run.sh --scene menu --headless --frame 8 --audio-wav build/menu.wav
ports/doom/run.sh --target native --scene e1m1
```

`TARGET=bf` (including the direct `bf` target and run-bf.sh) forces PCM_AUDIO=0
and MUSIC_AUDIO=0, excludes pcm_sound.c and music_sound.c from the source set,
and builds the runtime with AUDIO=0, excluding vm_audio.c and vm_music.c. Its
existing silent sound path has no new sample reads, mixing loops or PCM commands.
Setting PCM_AUDIO=1 or MUSIC_AUDIO=1 cannot override that BF rule.
Switching between ordinary and BF builds changes compiler flags and rebuilds
objects, so a cached audio-enabled image cannot be reused accidentally.

`MUSIC_AUDIO=0` is the default; set `MUSIC_AUDIO=1` explicitly when building to
enable experimental music. The Clang
reference provides pixel and SFX oracles; it has no VM music device. Actual FM
output is compared across the interpreter, JIT and RCC native runtime.

Validation:

- `scripts/test-music.sh`: compiled C exercises synthesis, loops, pause/resume,
  volume and stop, comparing exact WAV output across all three backends.
- `node ports/doom/tools/test-music.mjs`: title music and E1M1 music/gunfire,
  with exact cross-backend audio and Clang reference pixels.
- `scripts/test-pcm.sh`: exact C PCM output through interpreter, JIT and native.
- `node ports/doom/tools/test.mjs fixtures --program pcm_sound`: production
  decoder/mixer versus explicit sample values on host C and Ripple.
- `node --test ports/doom/tools/pcm-build.test.mjs`: BF source/flag exclusion.
- `node ports/doom/tools/test-pcm.mjs`: actual menu and E1M1 gunfire sound and
  pixels compared with the independent host C engine on interpreter, JIT and
  RCC native.
