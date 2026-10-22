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
| OUTPUT_STATE | 0x0205 | 6 | pending speaker frames u32 (out), live speaker u16 (out), reserved=0 |

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

OUTPUT_STATE is an ephemeral host-output query: it includes the software sink
queue and the estimated hardware tail, in frames at the configured guest rate.
`live` indicates an open speaker stream. It does not advance logical playback,
change the deterministic capture, or become part of checkpoint state. A muted
or unavailable speaker has no pending frames. Unix guests use this query to
avoid closing a stream before its final speaker buffer plays.

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

RVM's `--audio auto` defaults to speaker output for paced, non-headless runs.
`--audio on` also enables speakers for headless/unthrottled runs; `--audio off`
mutes speakers while preserving PCM processing. `--audio-wav FILE`
writes a standard signed 16-bit WAV, including in headless/unthrottled/stepped
runs. Headers are updated incrementally, so stopped runs retain valid captures.
One capture has one rate/channel format; changing it after writing samples is
an explicit error. RESET can start another logical segment of the same format.

The RCC native runtime uses `RCC_NATIVE_AUDIO=auto|on|off` and
`RCC_NATIVE_AUDIO_WAV=FILE`; its existing headless/capture and unthrottled modes
suppress automatic speaker playback, while `on` explicitly enables it. Console programs which never configure
PCM never initialize audio hardware. Checkpoints serialize the bounded guest
queue, logical cursor, remainder, gain and counters; speaker/file handles are
not copied into checkpoints.

## Unix PCM and WAV player

Fuse-X and Riplix expose `/dev/pcm` through the shared driver in
`ports/fuzix/ripple/pcm.c`; the userspace contract is
`ports/fuzix/ripple/include/sys/pcm.h`. Major 5, minor 0 provides an exclusive
process-owned output lease with exit/exec cleanup. Inherited descriptors cannot
control a parent's lease. The kernel validates user ranges and stages at most
1,024 frames in kernel memory per write before invoking the host device.

Writes count signed 16-bit sample cells, in whole mono/stereo frames. Partial
writes, blocking/nonblocking operation and select readiness are supported.
Ioctls configure the version/rate/channels/capacity, query frame counts and
flags, start/pause/stop playback, set gain, mark end of input, and wait for drain.
`PCM_END` requests drain without blocking, including before a short stream is
started. `PCM_DRAIN` waits for a running stream. On end of input, render calls
stop at the final queued sample; completion also waits for OUTPUT_STATE's
speaker backlog to reach zero.

These kernels use Unix monotonic clock updates to issue explicit RENDER calls;
they do not grant NEXT_TICK. The driver carries fractional frames, bounds a
single catch-up update to 250 ms, and exposes skipped time as `late_ms`. Empty
queues during normal running render silence and increment missing-frame counts.
Interactive Unix launchers enable speakers even for unthrottled RVM; headless
launches retain automatic mute unless explicitly given `--audio on`.

`music` is an RFL curses browser around a C streaming WAV engine. `wavplay` uses
the same engine from the shell. Both support PCM 8/16/24-bit mono/stereo WAVs at
8–96 kHz; 24-bit samples retain their upper 16 bits for the audio device.
The engine queues one second of audio, prefills three quarters of a
second (or all of a shorter file), and converts bounded 1,024-frame blocks.
See [the music player](../../ports/music/README.md) for usage, importing C16
files, sample-exact guest tests and the exploratory Tremor benchmark. Ogg/Vorbis
is not yet part of the player.

## SDL 1.2 applications

The [SDL port](../../ports/sdl/README.md#pcm-audio) adapts SDL callback PCM,
WAV loading, mixing and format conversion onto `/dev/pcm`. Callback data remains
logical octets and is converted into signed-16 sample cells before writes.
The transport keeps its existing version, exclusive ownership and monotonic
clock; SDL does not issue `NEXT_TICK` or explicit render commands.

The base SDL profile services callbacks cooperatively at SDL event, drawing and
wait/delay boundaries, with refill deadlines limiting sleep duration. It
supports ordinary SDL game loops, pause/resume and shared state protected by
`SDL_LockAudio`, but does not create an independent guest audio thread. Work or
blocking I/O outside those boundaries can underrun after the bounded queue is
consumed. Proper shared-address-space execution contexts remain a separate
prerequisite for fully asynchronous SDL callbacks. The native host speaker
thread consumes copied PCM only.

The optional [WAV effects mixer](../../ports/sdl/mixer/README.md) is an original
SDL_mixer 1.2-compatible source API subset, reusable by C applications and now
enabled for LGeneral effects. It provides eight default channels with checked
dynamic allocation, loops, volume, pause, expiry, fades and completion hooks.
Format/channel conversion and arbitrary-rate nearest-neighbor resampling occur
when WAVs are loaded. This leaves base SDL's power-of-two `SDL_AudioCVT` rate
conversion unchanged. Music streams and compressed decoders remain outside the
subset. The default local mixer retains cooperative scheduling; Fuse-X can
instead use the [independent mixer process](#audio-specific-backend-mixer-helper-over-ipc)
for uploaded effects. That backend does not make general SDL callbacks
asynchronous.

The focused suite passed 3,481 checks on host byte/wide carriers and actual C16;
production PCM captures matched overlap, finite-loop, volume and pause samples.
LGeneral's separate GUI/audio checks passed, including sample-exact original
clicks, scenario sound loading, persisted mute and close/reopen cleanup. These
captures establish sample correctness, not a physical speaker latency guarantee.
See the [SDL verification commands](../../ports/sdl/README.md#verification).

The soundboard `sdl-audio-demo.fxe` also exercises callback audio. Neither a new
audio device protocol nor startup-service changes are needed. LinCity-NG and
Wesnoth remain deferred at the user's request because of their C++ requirements
and other priorities; the WAV mixer does not resolve those dependencies.

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

## Physical speaker diagnostics

`DEVICE_*` audio operation `0x0206` (`OUTPUT_STATS`) uses a version-1, 12-cell
descriptor. Cells 2/3 return speaker-active/reserved-zero, 4–5 return recovered
starvation events (u32), 6–9 return missing host output frames (u64), and 10–11
return the longest recovered gap in milliseconds (u32). `vm_audio_output_stats`
wraps it. Riplix and Fuse-X expose the same data to the PCM owner through
`PCM_OUTPUT_STATS` and `struct pcm_output_stats`; the existing `PCM_STATE` ABI
is unchanged.

These counters observe the physical output callback, separately from logical
PCM queue underruns and the kernel's `late_ms` clock catch-up counter. An empty
interval is committed when PCM resumes. Silence before first playback, after a
final drain, or discarded by deliberate pause/stop is excluded. Configure starts
new counters. Statistics are ephemeral, absent from serialized checkpoints, and
queries do not advance the logical audio timeline. Muted output reports zeros.
The native scheduling regression has an opt-in silent real-speaker test; unit
tests drive the same sink consumer without audio hardware.

## SDL threading investigation

Source audit, 2026-10-21; this is a design recommendation, not an implemented
thread facility. Shared-image green threads are feasible without host guest-code
callbacks, SMP, ring 3 or timer-driven native stack switching. They are a runtime
and kernel milestone, rather than a small SDL backend change. Keep the existing
cooperative audio pump for general callbacks while investigating that work
separately. The finite-effects helper below supplies independent PCM production
without threads. More queued PCM alone can absorb a bounded delay; it cannot
guarantee arbitrary application-callback progress through a compute loop or
blocking call.

### What is available now

* [native_hosted.h](../../runtime/include/native_hosted.h) exposes kernel-selected
  clone/switch/drop/exec contexts, not thread creation. In the hosted
  [process runtime](../../rcc-native-runtime/src/process.rs), `clone_image` copies
  the private memory handle, stack cursor, allocation maps, RNG and errno address.
  [address_space.rs](../../rcc-native-runtime/src/address_space.rs) detaches private
  pages on writes. Sharing loaded code or the explicitly shared kernel allocator
  does not make application globals and malloc objects shared after fork.
* [Compiler continuations](../../rcc-native/src/process.rs) already preserve live
  values and resume PCs across calls and loop checkpoints. Public function tokens
  still name resumable entries, including for indirect calls; a thread entry must
  be validated with the right arity and retain its loaded module. The runtime
  currently owns one continuation stack per process, with logical frame storage
  in banks 2 through 4 and one stack cursor. Thread creation must reserve disjoint
  logical stack storage and separate frame/jump records, not copy a host stack or
  reuse the same logical frame addresses.
* Hosted `checkpoint_due` checks time every 256 candidate checks and requires
  roughly 5 ms since the previous grant. Calls return through the dispatcher;
  backward branches have generated checks. This is an opportunity to schedule,
  not a 5 ms callback deadline. Aggregate-return copying, active checkpoint or
  syscall continuations, and synchronous native helpers can defer it. In
  particular, the scan suppresses checkpoints while either kernel continuation
  remains anywhere in the active frame chain.
* [Fuse-X platform code](../../fuse-x/platform.c) saves a complete `u_data` per
  process slot before `rcc_hosted_switch`. The
  [kernel sleep path](../../ports/fuzix/upstream/Kernel/process.c) sleeps that
  process and resumes its parked syscall later. `u_data` mixes syscall arguments,
  I/O buffers, result/error and `u_insys` with process resources such as descriptors,
  brk and signal dispositions. A library-only fiber cannot run a sibling while
  its sole kernel process is asleep.
* [SDL support](../../ports/sdl/platform/support.c) explicitly rejects thread,
  mutex and timer creation. [audio.c](../../ports/sdl/platform/audio.c) calls the
  mixer only at cooperative boundaries and suppresses service while locked or
  already inside a callback. Its lock-depth counter is not a thread mutex. The
  [PCM backend](../../ports/sdl/platform/pcm_audio.c) already uses nonblocking
  writes and copied samples, which is suitable for an eventual audio worker.
* Ordinary `Runtime::poll` is expressly forbidden to switch contexts or invoke
  guest callbacks by the accepted memory-view contract. An audio hook there
  would break existing read-view lifetime guarantees. The scheduling checkpoint
  is the separate resumable boundary. Existing setjmp/longjmp also is not a fiber
  API: it targets a still-live continuation and rejects a returned frame.

### Options and relative scope

| Option | What it can achieve | Scope and limit |
| --- | --- | --- |
| Keep pumping; split long application work into explicit steps | Shared mixer state, correct callback pointers and refill between work chunks | Small application change; blocking libc and unbroken work can still starve audio |
| SDL-only cooperative task/fiber layer | Explicit scheduling among adapted tasks in one image | Useful only with explicit yields and nonblocking I/O; current public APIs cannot create a separate resumable C call stack |
| Separate mixer process with chunk/command IPC | Continue uploaded effects while the GUI process sleeps or computes | Implemented optional Fuse-X effects backend; private helper and bounded protocol, not shared-image threads |
| Kernel-selected sibling continuations sharing one process image | Run the audio worker while main computes or another sibling waits | Recommended long-term direction; requires shared-image ownership, separate continuations/stacks, kernel wait state and synchronization |
| Host OS thread entering guest code | No supported execution contract | Would violate exclusive runtime access and guest/kernel state assumptions; exclude |
| Forked arbitrary SDL callback or signal-driven mixer callback | No shared-image threading guarantee | COW separates mixer objects; signal delivery does not provide independent callback scheduling or allocator safety; exclude |

The smallest isolated prototype is a standalone continuation/ownership fixture
under a new `ports/sdl/threads/` directory, with no production imports or build
changes. A small header/implementation, two manually resumable test tasks and a
test runner can establish the proposed contract: one shared image/heap object,
distinct task locals and stack ranges, entry token plus userdata, per-task errno,
sleep/wake/join and a mutex wait queue. Use a fake monotonic clock and PCM sink to
exercise deadlines and partial writes deterministically. It must label simulated
syscall waits and hand-written continuations as a model; passing it would not
demonstrate arbitrary compiled SDL callbacks or working guest threads. A separate
application demo with explicit work chunks can exercise today's real SDL pump,
but also cannot claim independent threading. Neither experiment needs an x64
image, driver, compiler/runtime patch or hardware run.

### Audio-specific backend: mixer helper over IPC

The optional Fuse-X process backend is implemented for the finite `Mix_*`
effects profile. The parent uploads chunks and sends play/stop/volume/fade
commands; `sdl-mixerd` owns chunk/voice state and the sole `/dev/pcm` lease,
running the existing refill loop independently. Uploaded effects, including
looping effects and fades, progress while the GUI blocks in read/select or
computes between scheduler checkpoints. This is explicit interprocess audio
service, not a forked arbitrary callback or general thread support.

Select `library(..., mixer='process')` to link the client frontend. The default
`mixer=True` keeps the supported in-process backend. Build the matching helper
with `ports/sdl/build.py --app mixer-helper`; its output is `sdl-mixerd.fxe`.
The parent uses an explicit guest `SDL_MIXER_HELPER` path when set, otherwise
tries `/usr/bin/sdl-mixerd` then `/bin/sdl-mixerd`. LGeneral stages a matching
adjacent helper with its executable. No startup service or kernel change is
needed. See the [mixer profile](../../ports/sdl/mixer/README.md#optional-fuse-x-mixer-process)
for build, installation and callback semantics.

The implementation reuses existing process, pipe, nonblocking I/O and select
services. Two anonymous pipes carry versioned `MXP1` frames across exec; the
helper opens PCM only after exec, with unused pipe ends and extra descriptors
closed. Headers encode explicit 32-bit values, checked request/reply identities
and at most 1,024 logical octets of payload. Serialized octets remain `Uint8`
carriers on C16; no native structure, guest pointer or callable crosses IPC.
Large uploads are checked begin/data/commit transactions, and later commands
reference chunk IDs. The helper services audio before bounded IPC steps, uses
nonblocking replies and retains only bounded pending protocol state. A partial
request or a parent that stops reading replies does not block PCM refill.

Generic RWops reads, WAV decoding, conversion and arbitrary-rate resampling stay
in the parent at load time. Helper chunks are immutable sample snapshots, not
shared memory. Mutating `Mix_Chunk.abuf`/`alen` or its sample buffer after upload
is unsupported; volume changes use the supported mixer APIs. RAW storage stays
caller-owned and immutable until free. Free orders helper-side stop/release
before reclaiming the parent wrapper. Closing invalidates helper chunk IDs;
reopen requires loading chunks again, even at the same rate/format. Command
replies preserve selected-channel, previous-volume and live-state results.

Finished notifications return to the parent through nonblocking SDL-boundary
polling. The user hook runs only in the parent after SDL service or mixer calls;
it cannot run while that process is blocked or computing without a boundary.
Pending events coalesce per channel, and hook registration starts a fresh
notification interval by discarding older events. There is no unbounded
completion history and no callback execution from the helper.

EOF/error releases helper chunks, descriptors and its PCM lease. The client
reports framing, failed launch, acquisition and transport errors. A ten-second
deadline while synchronously waiting on IPC triggers controlled detach and
child cleanup/reaping, with bounded grace before killing an unresponsive
helper. Time spent on application work between SDL boundaries is not an IPC
waiting timeout. Close explicitly stops effects and requests exit. Failed
sessions do not restart automatically with stale chunk identities.

The process backend costs a process slot, private image/heap, process switches
and copied sample storage. It changes no shared runtime/compiler/kernel/syscall
or x64 integration code. It supplies no `SDL_CreateThread`, shared callback
userdata, asynchronous general `SDL_OpenAudio` callback, hard deadline or x64
sound-device support; another target needs its own packaging and acceptance.
Long synchronous kernel work can still delay both processes.

The dedicated [guest suite](../../ports/sdl/tests/mixer_ipc.py) captures PCM with
the parent blocked on an empty pipe or computing without SDL calls, then tests
partial/flooded IPC, unread replies, controls, failed launch, helper death and
abrupt parent exit. These use disposable guest images and check reclamation.
Capture continuity and queue behavior are separate from physical speaker
latency; a muted deterministic capture does not certify hardware deadlines.

### Contract required for actual sibling execution

1. **Share the process image, not a COW clone.** Keep globals, heap contents,
   allocator metadata, loaded-module ownership, descriptor table and brk under
   one process-group owner. Each thread needs its own saved PC/values/arguments,
   logical stack bounds/cursor, jump records, return value, identity and wait
   state. A thread return must finish that thread; today's outermost continuation
   return requests process/session exit. Function tokens and userdata remain C16
   values; retain the image until its last continuation and callback reference
   has gone. Stack addresses can be shared with another thread only while their
   owning stack is live; never restore copied stack bytes over a sibling's work.
2. **Let the existing kernel scheduler select runnable threads.** Extend its
   task identity to associate a thread with a shared process group; do not add a
   competing host scheduler or a second process run queue. A thread blocked in
   read/select/sleep needs its own saved kernel continuation, syscall request,
   `u_data` transient state, wait event and timeout. Switching siblings must not
   park/restore their common memory or heap. Copying all of `u_data` is inadequate:
   shared descriptor changes, brk, cwd and signal rules need explicit ownership.
   The [scheduler](../../ports/fuzix/ripple/scheduler.c) currently indexes its
   run/wait queues and timeouts by `ptab` slot; resource tables still assume that
   slot is the process. This is the main integration cost for blocking I/O.
3. **Preserve per-thread errors and protect shared libraries.** Guest libc has
   one global `errno`; the runtime saves its address, not a thread-local value.
   Saving/restoring that cell can bootstrap green-thread errno only if pointer
   escape semantics are restricted; a full libc contract needs thread-local
   access. SDL error buffers also need review. The guest
   [allocator](../../ports/fuzix/ripple/libc/heap.c) and
   [stdio](../../ports/fuzix/ripple/libc/stdio.c) mutate shared lists without thread
   locks; allocator growth calls brk. Single-CPU execution still permits a
   checkpoint between those mutations. Add scheduler-aware mutexes or bounded
   no-switch critical sections, including the brk transaction. Never yield/spin
   waiting for a lock whose owner cannot run, or sleep with a global no-switch
   guard held. A restricted first audio worker should preallocate buffers and
   avoid allocation, stdio and blocking I/O inside its callback.
4. **Define lifecycle and device ownership before general use.** Thread exit and
   join reclaim only the completed thread; process exit cancels/wakes all siblings
   and releases resources once. Exec must retire every old sibling before
   replacing the image. A safe first experimental policy can reject fork/exec
   while multiple threads exist; eventual fork must specify which continuation
   survives in the child and repair inherited locks. Define process-directed
   signal selection, thread masks, fatal group exit and delivery only after
   kernel resources are safe. Do not execute audio from the current signal
   handler path. The [PCM lease](../../ports/fuzix/ripple/pcm.c) is checked with
   `owner == udata.u_ptab` plus PID; a worker represented by a different task slot
   needs a shared process-owner identity. Per-thread close must not inadvertently
   reset another sibling's stream. Apply the same review to graphics and file
   cleanup.
5. **Keep checkpoint and memory contracts explicit.** Start with explicit yield
   and sleep at dispatcher boundaries, then allow the existing compiler
   checkpoints to select siblings after allocator/lock protection is proven.
   The currently active syscall may be parked; its frame chain must not suppress
   scheduling in another sibling. Preserve aggregate-return storage lifetimes,
   read-view barriers and synchronous helper borrows. This first design does not
   require arbitrary native-stack preemption and cannot promise a hard audio
   deadline through unbounded native helpers or long kernel operations.

### Stages and acceptance

The isolated fixture is a few new files and a contract test, not production
thread support. A working hosted prototype is a coordinated change across the
runtime dispatcher/image model, compiler/service or syscall bridge, kernel task
and wait state, and selected libc/SDL locks. Merely adding create/yield calls to
SDL would leave the difficult requirements unresolved. Start with two siblings,
explicit yield, preallocated bounded stacks, nonblocking PCM and restricted
lifecycle; then prove blocked-main/audio progress and checkpointed compute.
General SDL thread/mutex/semaphore APIs and unrestricted application callbacks
are a later compatibility milestone.

Headless acceptance should cover pointer-identical globals/heap and function
userdata, disjoint locals/stacks, nested indirect calls and aggregate returns,
errno preservation, lock-held checkpoints, join/self-join, capacity/allocation
failure, starvation accounting, repeated create/close, and exact reclamation.
Use a deliberately empty blocking pipe/select in the main thread while audio
refills, plus a compute loop with no SDL calls; these distinguish actual sibling
execution from cooperative pumping. Test callback pause/close, partial/EAGAIN
PCM writes, signals, exit and the chosen fork/exec restriction. Compare captured
PCM and queue underruns separately from physical speaker gap counters; muted
headless success cannot establish hardware latency.

X64 adoption is a separate owner-reviewed stage. Its
[runtime](../../tools/x86_64/boot/runtime.c) and
[process snapshots](../../tools/x86_64/boot/process.inc) also bind frames, heap,
stack and errno to a process, but use fixed capacities and copied active C16 RAM.
Its checkpoint grant follows a new 100 Hz timer tick, rather than the hosted
5 ms wall-clock gate. A sibling must retain common backing while changing only
thread state; ordinary process snapshot switching cannot provide that by itself.
Preserve C16 layouts, checked helpers, current read-v1 rejection and both host RCC
and portable native compiler compatibility. No ring-3, IRQ-stack or SMP change
is implied. The ownership agreement in
[native-memory-coordination.txt](native-memory-coordination.txt) delegates only
the isolated x64 process-memory subtree to that workstream; it does not authorize
SDL threading changes to existing runtime, compiler, kernel or x64 integration
files. The shared-image-thread investigation remains documentation only; the
implemented effects helper does not establish sibling execution or x64 hardware
acceptance.
