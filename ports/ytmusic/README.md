# YouTube Music guest work

The accepted [media roadmap](../../docs/networking/youtube-music.md) targets
self-contained Fuse-X and Riplix x64 clients. This directory currently contains
an [extraction feasibility experiment](extraction/README.md), not an installed
player. The first reusable codec audit lives in [ports/opus](../opus/README.md).

JavaScript obtains service metadata and solves changing player challenges.
Transport, demuxing, decoding and PCM submission belong in bounded C buffers;
decoded audio must not pass through the JavaScript heap.

Checkpoint, 2026-10-22: the compiler/runtime/QuickJS optimization pass and both
prepared-player fixture gates are tested. Measured end-to-end improvements are
small; cold YouTube.js preparation still exceeds 600 seconds. See the
[documented checkpoint](../../docs/networking/youtube-music.md#checkpoint-2026-10-22)
and [results receipt](extraction/performance-results-2026-10-22.json) before
resuming extraction work. No player has been installed on user disks.
