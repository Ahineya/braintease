# Unix music player

`music` is a curses file browser, persistent playlist and WAV player for Fuse-X and FUZIX/Ripple.
The RFL interface uses the existing curses and file-panel libraries. Streaming,
sample conversion, buffering and device control run in C inside the guest.
`wavplay` uses the same engine without the screen interface.

## Use

Boot either normal image with `scripts/run-fuse-x.sh` or `scripts/run-fuzix.sh`,
then run these commands in its shell:

```sh
music /music
music /music/demo.wav
wavplay /music/demo.wav
wavplay --info /music/demo.wav
```

The normal images have the player, `/dev/pcm`, and an original quiet eight-second
`/music/demo.wav`. New builds install both programs and the device; generate and
import the demo as below when preparing another image.

| Key | Action |
|---|---|
| Tab | Switch between the file browser and playlist |
| Up/Down, Home/End, Page Up/Down | Move within the focused pane |
| Enter | Open a directory, or add/play a file; play the selected playlist track |
| a | Add the selected browser WAV to the playlist without interrupting playback |
| A | Add all WAV files in the current folder, in filename order (not recursive) |
| d | Remove the selected playlist entry; stops it if currently playing |
| s | Toggle continuous smart shuffle |
| n / p | Next track / previous playback-history entry |
| Space | Pause/resume |
| Left/Right | Seek backward/forward ten seconds |
| + / - | Adjust volume by five percent |
| F10, Esc, q | Quit and release audio |

The screen fits 80×30 and requires at least 60×16. The playlist holds up to 256
distinct absolute paths. Enter on a browser file keeps the familiar immediate
play behavior and adds it to the queue. Tracks advance automatically after
their audio finishes draining. Normal mode stops at the end of the list.
Smart shuffle plays every entry before reshuffling indefinitely, avoiding the
same song on either side of a shuffle-cycle boundary when at least two tracks
exist. One-track shuffle repeats that track. Newly queued tracks join the
remaining shuffle bag; previous follows actual playback history (bounded to
256 entries), rather than selecting another random song.

Missing or invalid tracks are retained in the playlist and skipped on playback.
If no track can be opened, playback stops rather than retrying in a busy loop.
The playlist, volume, shuffle toggle, last folder and selected playlist row save
automatically to `$HOME/.musicrc` (root fallback: `/.musicrc`). A sibling exclusive
temporary file and rename preserve the old file if writing fails; state has mode
0600. Paths are hex encoded to preserve spaces, quotes and line breaks. Restore
validates the entire versioned file before applying it. Reloading restores the
queue and settings without starting audio; Enter or n starts playback again.
Save/restore failures appear in the status area; a final save failure is also
reported after leaving the terminal UI. The first launch uses 75% volume;
`wavplay` uses unity gain independently of these settings.

Supported files are RIFF/WAVE PCM (format 1), unsigned 8-bit or signed little-endian
16/24-bit, mono or stereo, 8,000–96,000 Hz. The 24-bit reader retains the upper
16 bits of each sample for the audio device, without rounding or dithering; it
validates all three input octets. Unknown chunks and odd chunk padding are
handled, including data preceding the format chunk. The reader validates RIFF
bounds, format, byte rate, frame alignment and sample octets. Float, 32-bit,
extensible WAV, compressed WAV, multiple data chunks and empty tracks are rejected.
WAV and Ogg are different containers; Ogg playback is not implemented yet.

## Add music

Guest files store one file octet per C16 cell. Copying a host file directly into
the backing image would use the wrong representation; the importer handles it.
Shut down the selected guest before importing. The importer takes both image
locks, validates the filesystem, saves a private backup and preserves other files.

Run from the repository root on the host:

```sh
python3 ports/music/demo.py build/music-tests/demo.wav
python3 ports/music/import.py --target native --disk fuse-x/build/ROOTFS.IMG build/music-tests/demo.wav
python3 ports/music/import.py --target ripple --disk build/fuzix/fuzix.img build/music-tests/demo.wav
```

Substitute your WAV paths to install them in `/music`. Names must be distinct
ASCII filenames of at most 30 characters. The initial import checks the RIFF/WAVE
signature; the guest reader performs full format validation when opening a track.

Both launchers support `--audio on|auto|off` and `--audio-wav OUTPUT.wav`.
Interactive Unix launches enable speakers, including the normally unthrottled
Ripple guest. Headless playback requires explicit `--audio on`. Capture works
with speakers off; a capture file can contain only one rate/channel format.
Without an available sound device, the runtime reports muted output and capture
can still be used. Use JIT, the default, for Ripple audio playback.

## Guest PCM device

Both kernels share `ports/fuzix/ripple/pcm.c` and the interface in
`ports/fuzix/ripple/include/sys/pcm.h`. `/dev/pcm` is major 5, minor 0, exclusively
leased to the opening process. Exit and exec release the lease; an inherited
descriptor cannot write to or reset its parent's audio. Only a validated copy in
kernel memory is passed to host-device DMA.

`write` counts signed 16-bit sample cells, with whole interleaved channel frames.
It accepts partial writes, stages at most 1,024 frames per call, supports
nonblocking `EAGAIN`, and wakes blocked/selecting writers as space becomes free.
The ioctls configure rate/channels/capacity, query state, start/pause/stop, set
gain, mark end of input, and wait for draining. All multiframe counts in the
configuration and state are frames; the exposed position is unsigned 32-bit.
`PCM_END` is nonblocking and may be set before starting a short, prefilled file.
`PCM_DRAIN` is for a running stream and waits for completion.

Fuse-X additionally supports `PCM_MUSIC_BANK`, `PCM_MUSIC_SCORE`,
`PCM_MUSIC_CONTROL` and `PCM_MUSIC_STATE` on the same PCM lease. These connect
the existing host OPL3 synthesizer to OS applications such as Doom. Bank and MUS
buffers are validated and copied into temporary kernel storage before submission;
only the owning process can control or query music. Configuration and exit reset
the synth with the PCM stream. Music is mixed on the ordinary PCM render timeline,
so it follows the same output rate and speaker/capture device. FUZIX currently
returns `ENOTTY` for these optional requests.

Fuse-X's optional `PCM_MIX` is a bounded stateless effect mixer: up to 16 mono
octet voices become at most 630 stereo S16 frames per request, using unsigned
16.16 positions/steps and 0..127 gains. Kernel copies isolate source spans from
the host DMA operation. The mixer preserves C signed division, panning and
saturation, updates voice positions, and does not advance the PCM clock or queue.

Unix monotonic clock updates explicitly render PCM; these kernels do not drive
audio through VM `NEXT_TICK`. Fractional frames carry between clock updates.
Catch-up is bounded at 250 ms per update and excess time is exposed as `late_ms`.
During playback an empty queue renders silence and counts missing frames. After
end of input, rendering stops exactly at the last queued frame, then waits for
the host speaker's software and hardware backlog before marking completion.
Pause/stop discard already-rendered speaker backlog under the host PCM policy.
The physical sink adds 40 ms of startup silence to absorb host scheduling jitter,
and opens the speaker while configuring a paused stream. This lead affects only
live output; deterministic PCM and WAV captures retain their exact sample data.

The C engine keeps a one-second host-device queue, starts after a three-quarter
second prefill (or all of a shorter track), and reads/converts blocks of at most
1,024 frames. It does not hold a complete track. Each UI iteration does bounded
refill work, waits up to 20 ms for input, and redraws progress at five Hz.

## Verification and performance

```sh
make -C fuse-x music "$PWD/fuse-x/build/apps/pcmcheck.fxe"
make -C ports/fuzix BUILD="$PWD/build/fuzix" music "$PWD/build/fuzix/pcmcheck.rxe"
python3 ports/music/tests/test_wav.py
python3 ports/music/tests/test_import.py
python3 ports/music/tests/test_guest.py --target both
python3 ports/music/tests/test_playlist.py --target both
python3 ports/music/tests/test_ui.py --target native
python3 ports/music/tests/test_ui.py --target ripple
scripts/test-pcm.sh
```

Build the optional `pcmcheck` fixture with the corresponding Makefile's absolute
build target (`$PWD/fuse-x/build/apps/pcmcheck.fxe` or
`$PWD/build/fuzix/pcmcheck.rxe`) before running `test_guest.py`.
The UI test also uses the terminal renderer built by `libraries/tests/test_qr.py`.

Reader tests compare exact samples for 24 format/layout cases and reject 18
malformed files. Real guest tests compare captures with independent Python
samples, including data-before-format and odd metadata chunks. The device fixture
exercises invalid and bank-crossing user pointers, queue-full readiness and
process ownership/cleanup. Playlist tests cover sequential order, hundreds of
shuffle cycles, cycle-boundary repeat avoidance, bounded history, edits, escaped
path round trips, large multi-buffer state files, corrupt-file rejection and
failed-save preservation. UI tests exercise browsing, pause, seek, volume,
playlist editing, automatic track transitions, bad-track skipping, continuous
shuffle across a cycle boundary, quit and state restoration after a guest reboot.
A short quiet live speaker test can be run explicitly with
`test_guest.py --target both --live build/music-tests/demo.wav`.

On the development Mac, 15-second 44.1 and 48 kHz stereo WAVs played through
FUZIX/RVM JIT with exact captures and zero missing frames. Native format/capture,
UI interaction and live speaker/drain checks also passed. Increasing kernel
staging from 512 to 1,024 frames removed the RVM refill shortfall in these tests;
this milestone did not require broad VM changes. These results do not guarantee
playback under arbitrary host load or for every supported rate.

The same 15-second stereo captures pass for 24-bit source WAVs on both targets,
with exact upper-16-bit output and no missing frames. The first two seconds of
the user's 48 kHz stereo 24-bit track also matched its independently computed
samples in the native guest. Both `music` and `wavplay` share this reader.

For unpaced guest measurements:

```sh
wavplay --benchmark /music/demo.wav
wavplay --benchmark-output /music/demo.wav
```

The first measures file reads and decoding. The second also stages samples
through the paused PCM queue, clearing it as needed. Neither measures concurrent
clock rendering or physical speaker consumption. A 15-second 44.1 kHz stereo
pipeline test ran at 5.79× source duration on RVM JIT after the batching change;
host load affects these measurements.

## Ogg/Vorbis exploration

[Xiph Tremor](https://gitlab.xiph.org/xiph/tremor) is a fixed-point Vorbis decoder.
Its historical [`nobyte` branch](https://wiki.xiph.org/Tremor) explores machines
with wide C characters, making it useful porting material for C16. That branch
is from 2012; it is an experiment, not the production decoder to ship. The
current main branch is a better starting point for maintained decoder behavior,
with wide-character changes ported deliberately.

The local audit is reproducible against an external checkout:

```sh
git clone --depth 1 --branch nobyte https://gitlab.xiph.org/xiph/tremor.git /tmp/tremor-nobyte
python3 ports/music/tools/ogg_audit.py --source /tmp/tremor-nobyte --benchmark
```

No decoder is vendored or linked into the player. The script copies source and
license into ignored build output, substitutes exact-width integer types,
specifies endianness and supplies the inline fallback. At commit
`f946f9acbf7aced75d3810a52c878e47680a18f6`, 7 of 12 audited units compile. Four
encounter unsupported `alloca`; mapping also encounters a parser issue. Full
porting needs bounded scratch workspaces, a systematic audit of 16-bit `int`
arithmetic and shifts, and explicit octet-versus-sample accounting in decoder
I/O. Compile success alone does not establish decoder correctness.

The probe executes the real 2,048-point fixed-point inverse MDCT in both guests,
with a checksum compared to independent host C. For 32 transforms on the
development Mac:

| Runtime | Measured time | MDCT-only stereo budget at 44.1 kHz |
|---|---:|---:|
| Fuse-X native | 254 ms | 1.46× real time |
| FUZIX / RVM JIT | 4,741 ms | 0.078× real time |

Two transforms produce 1,024 stereo frames. These measurements include the
probe's coefficient filling and sparse checksum, and exclude Ogg parsing,
codebooks, floor/residue decoding, overlap and output. This is one block size,
not a full Vorbis performance result. Native has limited remaining headroom;
RVM needs substantial arithmetic/compiler/VM optimization before conventional
stereo Vorbis playback is practical. Fixed-point still requires many 32-bit
operations and wider products on the 16-bit VM.

The next step is to profile the transform's generated instructions and arithmetic
helpers, then port a maintained decoder with bounded workspace and compare full
PCM against libvorbis/Tremor reference output. Test mono and low sample rates
early, then stereo and malformed/truncated streams. Keep decoding inside the
guest; a host codec device is not part of this design.

## MP3 exploration

[minimp3](https://github.com/lieff/minimp3) is a compact CC0 decoder with a
floating-point core, even when its output is 16-bit PCM. The scalar Layer III
path is an approachable native candidate; SIMD paths cannot be used by our C16
guest. [libmad](https://www.underbit.com/products/mad/) is another candidate for
RVM: it uses 32-bit fixed-point computation and is GPLv2. Fixed-point avoids
software float but still needs an audit of integer widths, octet operations,
wide multiplication and memory limits on this machine.

`tools/mp3_audit.py` compiles minimp3 into private guests and compares every
decoded sample with the unmodified scalar decoder compiled by host Clang. It
does not add MP3 support to `music`, and does not call a host decoder from the
guest. The MP3 input is a small local test file, and sample output is extracted
only after shutting down the private guest. Its timing includes reads, decode,
validation and checksum/output conversion; diagnostic file writes are excluded.

The audited minimp3 revision is
`ea99364f61c14656440e8d77e9c233ccf3124633`, with header SHA-256
`57e437c5c1f0e8b243885d3929c8973b5e6c778451e0100ab4251d19915cb3ad`.
Keep its `minimp3.h` and `LICENSE` together in an external directory, then run:

```sh
ffmpeg -i build/music-tests/demo.wav -t 1 -codec:a libmp3lame -b:a 128k -write_xing 0 -map_metadata -1 build/music-tests/probe.mp3
python3 ports/music/tools/mp3_audit.py --source /path/to/minimp3 --mp3 build/music-tests/probe.mp3 --frames 6
```

The audit widens C `int`/`unsigned` to explicit 32-bit types and fixes three C16
promotion assumptions: byte-cache multiplication, literal shifts such as
`1 << 30`, and subtraction from the unsigned character carrier in the gain
exponent. Merely changing typedefs is insufficient. It also exposed an RCC
initializer bug: integer values in floating-point static tables were stored as
integer bit patterns. The compiler now inserts the destination's floating
conversion for scalar, array, struct and first-union-member initializers, with
a frontend regression test. The decoder's tables now retain their intended
values without editing their coefficients.

After that fix, all 13,824 PCM samples from six MP3 frames (6,912 stereo PCM
frames) matched the unmodified host decoder exactly on both Fuse-X and RVM JIT.
Observed throughput was 0.53–0.86× real time on native and 0.006–0.007× on RVM.
These short runs took place during other development/build work, so they are
rough feasibility measurements rather than isolated performance benchmarks.
They do establish that this scalar floating-point path needs optimization
before real-time use, especially on RVM. The earlier Tremor result measures only
an MDCT stage and must not be compared as if it were a complete Ogg decoder.

This is one synthetic 44.1 kHz stereo 128 kbit/s input, not conformance coverage
or a production port. A real MP3 integration needs a bounded incremental reader,
metadata handling, malformed/truncated input tests, more reference vectors,
seeking and an explicit gapless-playback policy. Throughput must be measured
again on longer files, including mono and lower rates, before promising playback.

Recommended next work is to profile banked memory access, 32-bit arithmetic and
wide products, plus the MP3 path's software floating-point helpers. Use those
results to improve generated code/VM execution and compare a maintained Tremor
port with a fixed-point MP3 candidate. Keep decoder input/output as guest octets
and PCM samples; neither path requires a host codec service.
