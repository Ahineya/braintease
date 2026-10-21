# Source provenance

`upstream/cproc` contains the top-level C sources, headers, README and license
from <https://git.sr.ht/~mcf/cproc>, commit
`d1c53ddf56571573a7025324c8dd5c6d547a4d1f` (2026-06-01).
The original source files are unchanged. Preserve `upstream/cproc/LICENSE`
when redistributing them. It includes ISC and MIT notices.

`adapt.py` generates local C16 frontend adaptations in the ignored build
directory. `src` supplies the RCC IR backend and driver. The upstream QBE
backend, driver and preprocessor implementation are not our guest toolchain.
`preprocessor.c` is the local replacement for upstream `pp.c`; it shares the
scanner, token interning and utility interfaces. Generated scanner changes add
string-backed input and explicit include-boundary handling without changing
the pinned sources.

`ripple/` is the local portable RCC IR reader and Ripple code generator. It
implements the existing Ripple calling convention, ISA and linker metadata
contract; it does not incorporate cproc's QBE backend.

Guest builds link the existing Riplix/Ripple startup and libc. `guest_build.py`
generates an extended, namespaced copy of `ports/fuzix/ripple/libc/format.c`
under `build/guest-support`, preserving its GPL-2.0-or-later notice and using
the existing floating-point formatter include. Those sources retain their
own licenses; the cproc license does not replace them. `guest/strtoull.c` and
`guest/metrics.c` are local bootstrap support. `src/strtod.c` supplies compiler-local
hexadecimal floating conversion, forwarding decimal input to the SDK. `source.py`
packages the adapted sources and their license notices for guest Make/Tcl builds;
`bootstrap.tcl` is the local build coordinator.

`guest/include/stdio.h` supplies compiler-local buffered getc/putc macros for
the Ripple SDK FILE layout. Refills, pushback, mode changes and flushes retain
the original libc functions. The generated source bundle includes the matching
SDK header, with its license notice; shared libc sources are not replaced.

`upstream/qbe` contains the complete, unchanged QBE 1.3 release from
<https://c9x.me/compile/release/qbe-1.3.tar.xz>, release commit
`c0818978acec60ebb6167fade60fb7012cbf20ca`. Archive SHA-256:
`d587905d620dc5e1d2bfa7c2cc642b9b837aa89a3188c6e37b53d756cf66e320`.
All 153 release files were compared byte for byte with the archive. Preserve
`upstream/qbe/LICENSE`, including Quentin Carbonneaux's MIT notice.
`qbe/adapt_qbe.py` generates C16 changes outside the vendor tree. QBE sits below
our RCC IR/continuation lowering; cproc's original platform ABI backend is
still not our C16 backend.

`native/` and `x64/` supply local checked native lowering and an ELF64 assembler.
`native/package.py` uses existing x64 bootstrap linking/packaging interfaces;
it is a host utility, not a guest linker.

`native-sdk/build.py` copies the existing Riplix, FUZIX, Fuse-X and Unicode
library sources/headers with their notices and the upstream GPL/LGPL texts.
Its source copies correct the `utime` definition's missing const qualifier and
cast `strrchr`'s const traversal result to its declared C return type. The
generated 64-bit integer formatter uses the adaptation described above. No
vendored/shared library file is edited by SDK preparation. Separately, the shared
FUZIX-derived `Library/libs/qsort.c` now uses a C16-safe, reentrant heapsort,
retaining the original attribution. This repairs 16-bit index/product overflow
and the old 128-cell record restriction for all source-library consumers.
`native-sdk/cells.c` supplies a local
checked-memory C fallback; the generated startup uses the existing kernel
argument and syscall contracts.
