# SDL_mixer-compatible PCM effects profile

`mixer.c` and `SDL_mixer.h` are an original implementation of a bounded subset
of the SDL_mixer 1.2 source API, licensed under LGPL-2.1-or-later. They are not a
copy or release of upstream SDL_mixer and do not claim complete compatibility.
The API names and general channel semantics follow the
[SDL_mixer 1.2 documentation](https://www.libsdl.org/projects/old/SDL_mixer/docs/SDL_mixer.html).
The linked SDL 1.2.15 PCM WAV loader and format/channel converter retain their
own upstream notices; see `../PROVENANCE.json` and the generated SDL notices.

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

Finished hooks run after a whole render block or state mutation has released
all internal chunk/voice pointers, while audio is locked. Hooks can stop/free
chunks, resize channels or close safely. Reentrant notifications are flattened
and dispatch at most the allocated channel capacity in queued notifications
per service boundary;
applications should avoid callbacks that continually restart and halt voices.
Replacement, halt, shrink, free and close notify for previously active voices.
Notifications caused by a render block are delivered after all its frames.
`Mix_GetChunk` retains the last chunk after completion/halt, until the chunk is
freed or the channel is replaced. `Mix_QuickLoad_RAW` borrows storage, and its
caller must retain that storage until it is no longer playing and the chunk
wrapper is freed. Closing releases channel state without freeing caller chunks.
Existing chunks remain tied to the format/rate in which they were loaded;
reload them when reopening with a different device specification.

Music streams, Ogg/MP3/FLAC/module decoding, compressed WAV, effects/postmix
hooks, positional audio and grouping are outside this profile. Common music
load/play entry points fail with an explicit SDL error. Other absent APIs are
a compile/link capability boundary. This library inherits SDL's cooperative
callback scheduling and exclusive PCM device lease; it introduces no audio
thread, host protocol or kernel change.
