# Opus decoder port

The first compile audit targets unmodified libopus 1.6.1 and the C16 native
frontend. It is a prerequisite for the [guest media plan](../../docs/networking/youtube-music.md),
not an installed decoder or a claim of real-time playback.

Download the official archive identified in `UPSTREAM.json`, then run:

```sh
python3 ports/opus/audit.py --archive /path/to/opus-1.6.1.tar.gz --matrix
```

Source and diagnostics stay under ignored `build/opus-audit`. Cached source
changes are rejected. Seven selected units cover range decoding, FFT/MDCT,
CELT, SILK's decoder API and the top-level Opus decoder. Each compiles with
upstream's `NONTHREADSAFE_PSEUDOSTACK` configuration. VLA and alloca modes
currently stop at frontend array/declaration diagnostics.

The broader `--full` audit also passes: all 129 generic fixed-point library
translation units from upstream's source lists compile through RCC, including
the encoder. This establishes frontend compatibility only; it does not establish
correct integer semantics, a working linked library, or decoding performance.

```sh
python3 ports/opus/audit.py --archive /path/to/opus-1.6.1.tar.gz --full
```

The scratch mode requires serialized decoder calls within a guest process.
It must not silently become a thread-safe library ABI. Audit alignment, scratch
bounds, allocation failure and cleanup before exposing it. Native LP64 builds
should use ordinary upstream allocation configurations and their own binaries.

Next: compile/link the reachable fixed-point decoder, audit 16-bit integer and
logical-octet assumptions, compare every PCM sample with the same upstream
configuration on the host, then measure sustained decoding and cancellation.
Cover SILK/CELT/hybrid, mono/stereo, loss concealment and invalid packets before
adding shared-library packaging or WebM demuxing. Preserve `COPYING` and source
notices when distributing library code.
