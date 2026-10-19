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

Guest builds link the existing FUZIX/Ripple startup and libc. `guest_build.py`
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
