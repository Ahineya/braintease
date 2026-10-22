# HP graphics: first native GX backend

`boot/gx.c` provides the existing GX transport over bounded native RAM surfaces.
The C16 kernel continues to own `/dev/gfx`, Unix descriptors and window leases;
`gx_call` accepts only numeric arguments and callback-validated C16 cell spans.
Legacy host-create format 0 remains RGB565, matching the existing hosted
backend and older frozen kernels. Handles carry generations, and mutating/data-reading calls require their native
context owner. Kernel-wide readiness polling is the sole handle-only operation.

This first slice displays one active surface at a time, with no compositor or
window chrome. It supports RGB565/XRGB8888 windows, RGB565/BGR logical-octet
uploads, scaled/indexed uploads, fill/copy, configure, explicit present, raw HID
make/break events and state. Pointer movement, three button edges and signed
16.16 wheel events use the same GX event stream. Alt+F4 sends GX_CLOSE, so an
existing LVGL app exits normally without an SSH kill. Native framebuffer conversion and console handoff
belong to the platform callback. Upload/fill/copy never publish implicitly.
BGR blend matches the existing hosted scalar primitive. Optional retained/A8
raster acceleration returns unsupported, selecting the
LVGL port's existing software fallbacks. It does not introduce another UI model.

Build with the already prepared LVGL adaptation and its read-only object cache:

```
python3 tools/x86_64/graphics/build.py
python3 tools/x86_64/graphics/check.py
```

The isolated RXE1 outputs are `build/x86_64-graphics/apps/lvcalc.fxe` and
`screenshot.fxe`. The calculator requires the native runtime's 32 MiB image limit.
No shared compiler, SDK, kernel or LVGL port is rebuilt or modified by this tool.
A clean checkout first needs an ordinary LVGL adaptation; `--lvgl-base` selects
it. This first recipe uses the existing `build/lvgl/system-dialogs/apps` cache.

## Screenshots

On the native system:

```
screenshot /tmp/screen.ppm
screenshot --scale 4 /tmp/preview.ppm
```

Retrieve that file with SFTP. It is a standard binary P6 RGB PPM once exported.
The optional scale is an integer from 1 to 16; it keeps every Nth pixel/row for
a smaller development preview. Default capture keeps the full resolution.
One RGB octet occupies one C16 char cell on the guest filesystem; SFTP exports
its low eight bits. The native display service supplies RGB888 logical octets,
independent of GOP RGB/BGR channel order and framebuffer row padding.

The private development syscall is `call(0x7847, operation, row, data, capacity)`:
operation 0 returns width, 1 height, and 2 writes one complete RGB row and returns
its sample count. Errors return -1. The native runtime intercepts this request;
it does not add a shared C16 kernel syscall. Ordinary rendering never reads the
framebuffer. Explicit screenshots may read it and capture rows successively, so
moving content may change during a capture. They are diagnostic snapshots, not
atomic frame recordings.
