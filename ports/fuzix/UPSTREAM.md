# Upstream source

- Project: [FUZIX](https://codeberg.org/EtchedPixels/FUZIX)
- Branch: master
- Revision: `0a2336cf8ce57cb94ad82f0c9f4cde2b4d578a78`
- Fetched and verified against the branch API: 2026-10-16
- Archive: `https://codeberg.org/EtchedPixels/FUZIX/archive/0a2336cf8ce57cb94ad82f0c9f4cde2b4d578a78.tar.gz`
- Archive SHA-256: `6dfaab209c84191972e69a8b258ccd667feee6a3f50d8298fe31e8a56ed03ee2`

The **complete, unmodified archive** is committed in `vendor/`. The portable
working tree in `upstream/` contains the kernel C sources, kernel/device headers,
minimal memory-allocation stub, README, credits, version, and license used by
this port. This avoids losing upstream `.S`/`.s` pairs on macOS filesystems and
avoids treating upstream's tracked generated assembly as ignored local files.
To explore/build other upstream platforms, extract the full archive on a
case-sensitive filesystem. No network fetch is required to build this port.

Seven core adaptations are recorded in `upstream.patch`, with paths relative to
`upstream/`:

- `Kernel/blk512.c`
- `Kernel/devio.c`
- `Kernel/include/kernel.h`
- `Kernel/inode.c`
- `Kernel/usermem.c`
- `Kernel/process.c`
- `Kernel/filesys.c`

These adapt block geometry/directory layout to C16, use sizeof for scalar
user-memory validation, avoid treating 16-bit int as 32-bit in kernel decimal
formatting, retain dirty buffers on failed writes, and add the VM durability
barrier to kernel sync. The panic path deliberately avoids marking a damaged
filesystem clean. Everything else is in `ripple/` and `tools/`.
