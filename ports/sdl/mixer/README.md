# SDL_mixer-compatible PCM effects profile

This directory is an original implementation of a bounded subset of the
SDL_mixer 1.2 source API, licensed under LGPL-2.1-or-later. It is not a copy or
release of upstream SDL_mixer and does not claim complete compatibility.
The API names and general channel semantics follow the
[SDL_mixer 1.2 documentation](https://www.libsdl.org/projects/old/SDL_mixer/docs/SDL_mixer.html).
The linked SDL 1.2.15 PCM WAV loader and format/channel converter retain their
own upstream notices; see `../PROVENANCE.json` and the generated SDL notices.

Two backends expose the same effects API. The default in-process `mixer.c`
uses the existing cooperative SDL audio pump. Fuse-X applications can instead
link `client.c` and run the mixer in the independent `sdl-mixerd` guest process.
The helper uses the same mixing engine and owns its own PCM lease; uploaded
effects keep playing while the application blocks or computes outside SDL.
The process backend is optional and does not change the default local path.

The profile provides PCM WAV loading, borrowed raw PCM chunks, eight default
effect channels, checked dynamic allocation, reservations,
finite/infinite loops, channel/chunk volume, pause/resume, expiry, fades and
channel-finished notifications. WAV decoding accepts the base SDL profile's
uncompressed 8/16-bit mono/stereo PCM input. Format/channel conversion uses
`SDL_AudioCVT` at the source rate. A mixer-local nearest-neighbor resampler
handles arbitrary input/output rate ratios at load time. Its output contains
`floor(input_frames * output_rate / input_rate)` complete frames and samples
source frame `floor(output_frame * input_rate / output_rate)`.

The actual obtained device format determines chunk storage and mixing. Device
requests support unsigned/signed 8-bit and either-endian 16-bit PCM, mono or
stereo, at 8–96 kHz. Frequencies and millisecond durations use `Sint32`;
chunk lengths count logical
octets in `Uint32`. Callers must compile against this port's matching headers.
Serialized audio uses one `Uint8` carrier per octet even on C16. Packed audio
is never reinterpreted as scalar sample pointers.

Mixing accumulates every voice into signed 32-bit sample sums and clips once
per output sample. This deliberately avoids upstream SDL_mixer's channel-order
clipping. Channel volume and chunk volume combine using integer truncation.
The public signed 16-bit channel/count ABI bounds allocation to 32767 voices;
`32767 * 32768 = 1073709056` bounds the signed 32-bit sample accumulator.
Allocation products are checked before allocation, and exhausted memory fails
without changing existing voices. Playback and
fades advance by complete rendered frames; pause freezes playback, expiry and
fade progress. Millisecond durations round up to a complete output frame and
saturate at the internal unsigned 32-bit frame counter's maximum if necessary.
Fade-in starts at zero gain and reaches full gain after its duration; fade-out
starts at its current fade gain and stops after its duration. A new volume
setting applies to the channel's underlying volume while its fade continues.
Expiry with zero ticks stops immediately; negative ticks cancel expiry.

## In-process lifetime and callbacks

With the default backend, finished hooks run after a whole render block or
state mutation has released
all internal chunk/voice pointers, while audio is locked. Hooks can stop/free
chunks, resize channels or close safely. Reentrant notifications are flattened
and dispatch at most the allocated channel capacity in queued notifications
per service boundary;
applications should avoid callbacks that continually restart and halt voices.
Replacement, halt, shrink, free and close notify for previously active voices.
Notifications caused by a render block are delivered after all its frames.
`Mix_GetChunk` retains the last chunk after completion/halt, until the chunk is
freed or the channel is replaced. `Mix_QuickLoad_RAW` borrows storage, and its
caller must retain that storage without mutation until it is no longer playing
and the chunk wrapper is freed. Closing releases channel state without freeing
caller chunks.
Existing chunks remain tied to the format/rate in which they were loaded;
reload them when reopening with a different device specification.

Music streams, Ogg/MP3/FLAC/module decoding, compressed WAV, effects/postmix
hooks, positional audio and grouping are outside this profile. Common music
load/play entry points fail with an explicit SDL error. Other absent APIs are
a compile/link capability boundary. The default backend inherits SDL's
cooperative callback scheduling and exclusive PCM device lease. Neither backend
introduces general guest threads, host calls into application callbacks or
kernel changes.

## Optional Fuse-X mixer process

Select the backend at link time through the reusable SDL builder:

```python
library('fuse-x', build, compiler, mixer='process')
```

`mixer=True` retains the in-process implementation. Build the matching helper
from the same source revision:

```sh
python3 ports/sdl/build.py --app mixer-helper --build build/sdl-mixer-helper
```

This produces `build/sdl-mixer-helper/sdl-mixerd.fxe`. Install it as
`/usr/bin/sdl-mixerd` in the guest, or `/bin/sdl-mixerd` as the fallback. Set
`SDL_MIXER_HELPER` to an explicit guest executable path to use another location;
an explicit path does not fall back silently. LGeneral stages the helper beside
its executable in `fuse-x-shared` and supplies that adjacent helper path. There
is no boot service to configure: `Mix_OpenAudio` launches one private helper for
the owning application, and `Mix_CloseAudio` shuts it down. Missing executables
or a busy PCM lease cause an SDL error; they do not silently switch backends.

### Transport and sample ownership

The parent forks/execs the helper with two anonymous pipes. It closes unused
ends and extra descriptors; only the helper acquires `/dev/pcm`, after exec.
The protocol is private and versioned (`MXP1`), with explicit 32-bit scalar
fields, request/reply IDs and at most 1,024 logical octets of payload per frame.
One serialized octet is one `Uint8` carrier even on C16. Native structures,
guest pointers and callable addresses never cross the process boundary.

WAV/RWops decoding, format conversion and arbitrary-rate resampling happen in
the parent at load time. Upload uses checked begin/data/commit transactions;
only a complete committed chunk can play. Repeated playback refers to helper
chunk IDs rather than copying samples again. The helper pumps audio between
bounded command/reply steps, uses nonblocking reads/writes and waits only until
its next refill deadline. A partial command or an undrained reply cannot block
PCM production; retained protocol state and pending completion flags are bounded.

Playback uses an immutable snapshot uploaded by `Mix_LoadWAV_RW` or
`Mix_QuickLoad_RAW`. Editing the parent chunk's sample buffer or changing
`abuf`/`alen` after upload does not update the helper. Use `Mix_Volume` and
`Mix_VolumeChunk` for supported volume changes; load a new chunk for different
samples. RAW storage remains caller-owned and must remain immutable until
`Mix_FreeChunk`. `Mix_FreeChunk` sends ordered stop/release before freeing the
parent wrapper. Closing audio releases all helper-side chunks and invalidates
their IDs; free old wrappers and load chunks again after reopening, even with
the same device format. A forked child cannot control the parent's helper
session.

### Completion and failure behavior

The helper performs playback and fades independently, but application callbacks
still run in the parent at SDL service boundaries or after mixer commands.
They do not run during a parent's blocking read or uninterrupted computation.
Completion flags coalesce per channel until serviced, rather than forming an
unbounded event log: several completions on one channel can produce one hook
call. Registering/replacing `Mix_ChannelFinished` starts a fresh notification
interval and discards older pending events, including events accumulated while
no hook was installed. Reentrant dispatch is bounded and does not call a hook
from the helper process. This is a notification contract, not arbitrary
asynchronous execution of application code.

The client validates reply IDs, framing and error responses. IPC failure or a
10-second deadline while synchronously waiting for IPC closes the connection
and performs controlled child cleanup/reaping; application computation between
SDL boundaries is not itself a timeout. A stuck helper is killed after a
bounded grace period. Parent exit closes its command pipe, and helper EOF/error
releases chunks, descriptors and its PCM lease. Successful close explicitly
stops effects and requests helper exit. The helper is not a persistent server
and is not restarted automatically with stale chunk IDs.

This consumes a process slot, an executable/heap and copied sample storage in
addition to the parent. It remains subject to the existing scheduler: long
synchronous kernel work can delay both processes. It supplies no
`SDL_CreateThread`, asynchronous general `SDL_OpenAudio` callback, shared
callback userdata, compressed music decoding or x64 audio backend. Another
target needs its own helper packaging and acceptance.

Process-profile verification uses disposable guest images and deterministic
PCM captures:

```sh
python3 ports/sdl/build.py --app mixer-ipc --build build/sdl-mixer-ipc
python3 ports/sdl/tests/mixer_ipc.py --guest build/sdl-mixer-ipc/sdl-mixer-ipc.fxe --helper build/sdl-mixer-helper/sdl-mixerd.fxe
```

The suite distinguishes actual independent progress from cooperative pumping:
it holds the parent in a blocking pipe read or compute loop without SDL calls,
then exercises partial commands, unread/flooded replies, controls, failed
launch, helper death and abrupt parent exit. Compare captured sample continuity
and cleanup separately from physical speaker latency.
