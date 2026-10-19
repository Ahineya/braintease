# Lynx C16 browser

Lynx runs inside FUZIX on RVM and native Fuse-X, using guest libc, shared C16
curses and the guest's own DNS/TCP stack. The configured upstream Lynx/libwww
units and small port adapter compile with RCC. HTTP rendering, link navigation,
GET forms and Telnet browsing have actual guest acceptance tests. See the
[roadmap](../../docs/networking/terminal-browser.md) for broader web milestones. HTTPS now uses the shared guest wolfSSL client,
including the optional [crypto accelerator](../../docs/arch/crypto.md).

## Build and test

Normal `scripts/run-fuzix.sh` and `scripts/run-fuse-x.sh` builds include Lynx in
their usual application inventory. They install `/bin/lynx`, its help page and
license, and the default `/etc/lynx.cfg`, preserving an existing configuration.
After starting the guest, run `lynx` or `lynx http://wiby.me/`.
`--build-only` also builds/installs it; `--run-only` keeps the existing apps, and
an explicit `--disk` still requires `--install-apps` to update its contents.
`FUZIX_BUILD` and `FUSE_X_BUILD` select the normal target-specific output paths
(`lynx.rxe` and `apps/lynx.fxe` respectively). Unchanged builds reuse the normal
Make/application cache.

For standalone browser development and testing:

Build the workspace compiler/runtime and the normal Fuse-X kernel, then run:

```sh
python3 ports/lynx/build.py
python3 ports/lynx/test_local.py
```

The result is `ports/lynx/build/lynx.fxe`, with roughly 308,000 static cells
across five banks and a 39 MiB package, including guest TLS. `build/resources-native.json` records
exact sizes and function counts for the latest build.
The compiler selects the extended global region automatically. Native ABI 2
supports sixteen global banks; ABI 1 packages remain loadable. Rebuild the
kernel/runtime before running these larger packages.

For RVM/FUZIX, build the normal kernel/userspace artifacts first, then:

```sh
make -C ports/fuzix BUILD="$PWD/build/fuzix" -j4 images
python3 ports/lynx/build.py --target ripple --build-dir build/fuzix
python3 ports/lynx/test_local.py --target ripple --build-dir build/fuzix
```

This lowers all browser units with bank-aware global references and links the
shared guest SDK, including curses, software floating math and floating output
formatting. The result is `ports/lynx/build/lynx.rxe`, with roughly 5.5 million
instructions and 308,000 static cells, including guest TLS. `build/resources-ripple.json` records
exact sizes for the latest build. The
linker also emits `build/ripple-map.json`, mapping symbols to instruction indices
and dense data offsets for debugging; `rlink --map PATH` is available generally.

`test_local.py` uses a disposable disk with networking disabled. Network/UI tests
below use their own disks and independent Ethernet fixture; ordinary user images
and running sessions are preserved. Per-unit diagnostics and resource reports
live in `build/`.
`--skip-compile` reuses the selected target's existing IR; use it only when the
sources and reviewed configuration have not changed.

To try the native renderer manually, copy the package and fixture files into a
host share, launch Fuse-X with `--share /absolute/folder`, and run:

```text
/mnt/host/lynx.fxe -dump -nolist -cfg=/mnt/host/lynx.cfg /mnt/host/local.html
```

The browser uses guest libc and the guest filesystem. The host compiles its
machine code but does not render the page.

## Install and browse

The launchers install Lynx with the other apps. For a standalone package built
as above, an optional installer can update a stopped guest's existing disk
(use the actual disk path):

```sh
python3 ports/lynx/install.py --target native --disk /path/to/native-root.img
# Or the outer RVM disk containing ROOTFS.IMG:
python3 ports/lynx/install.py --target ripple --disk /path/to/fuzix.img
```

The installer refuses a locked disk, validates the filesystem, saves a backup,
installs `/bin/lynx` and a local help page, and preserves an existing
`/etc/lynx.cfg`. Repeat installation is idempotent. It never installs the native
package into RVM or vice versa. Rebuild/update the matching kernel and userspace
before using these bank-spanning packages.

After restarting the guest, run:

```text
lynx
lynx http://wiby.me/
lynx -dump http://example.com/
```

Networking initializes through the normal guest network service. `g` opens a
URL; Ctrl-U clears it. Arrows/Tab select links and fields, Right/Enter follows,
Left goes back, Space/b scrolls, `z` cancels a transfer, Ctrl-L redraws, and
`q` then `y` exits. Text fields and select controls submit GET forms with normal
URL encoding. The same interface works on a Telnet PTY.
Ctrl-Z restores the shell; `fg` resumes and repaints the browser. The port owns
this signal handling because the shared curses library leaves job control to
applications.

## Startup and terminal performance

The October 18 console/loader changes address two independent costs. Fuse-X's
root block driver now converts little-endian C16 cells in the host I/O adapter,
and its executable streamer copies whole memory spans instead of acquiring the
shared kernel-memory lock for every package octet. Files still go through the
guest filesystem and permission checks; native applications cannot call these
kernel-only adapters directly.

The shared console line discipline now batches ordinary output through
[`TTY_WRITE`](../../docs/arch/mmio.md), using bounded kernel staging on RVM and
Fuse-X. Existing buffered libc/curses applications benefit without rebuilding
the application. Standalone runtime `puts`/`printf` also use bulk output, with
legacy fallback; PTYs retain their existing queue/backpressure behavior.

On this development host, medians of three runs using the **same installed
39 MiB TLS-enabled browser binary**, networking disabled and a local fixture:

| Fuse-X measurement | Before | After |
| --- | ---: | ---: |
| `lynx -version`, including process exit | 6.27 s | 2.32 s |
| Local HTML dump, including process exit | 7.15 s | 3.61 s |
| Local interactive page, through the final link cursor | 7.94 s | 3.62 s |
| 128 KiB raw console output, excluding executable loading | 696 ms | 26.9 ms |

The last measurement is transport throughput, not a claim that every editor
operation becomes 26 times faster. The equivalent RVM/JIT console probe recorded
285 ms versus 133 ms medians, with substantial cold-JIT variation. Network, DNS
and TLS latency are excluded. Remaining startup includes loading the large
package and the browser's initialization/rendering work.

Reproduce on disposable disks, without concurrent builds:

```sh
python3 ports/lynx/bench_startup.py
python3 fuse-x/cc.py ports/fuzix/tools/consolecheck.c -o fuse-x/build/apps/consolecheck.fxe
python3 ports/fuzix/tools/test_console_output.py --os fuse-x --bench
make -C ports/fuzix BUILD="$PWD/build/fuzix" "$PWD/build/fuzix/consolecheck.rxe"
python3 ports/fuzix/tools/test_console_output.py --os fuzix --bench
```

`bench_startup.py --kernel /path/to/saved/fuse-x` compares a saved kernel with
the same current apps. The console probe accepts `--kernel` too (a Fuse-X binary
or RVM binary according to `--os`). Omit `--bench` for exact raw-octet,
bank-crossing and stdio ordering checks. Neither tool opens a normal user disk.

## Bounds and unsupported features

The port caps each HTTP response at **65,536 wire octets**, including the status
line, headers and chunk framing. The cap applies without Content-Length and to
chunked responses (after TLS decryption for HTTPS); overflow aborts the load with an explicit message and leaves
the browser usable. This is a transfer bound, not a guarantee about the memory
cost of every possible HTML document. Cache defaults to two rendered documents;
connect/read waits default to 15 seconds. `/etc/lynx.cfg` holds those defaults
and a 2,048-character URL limit. Request writes handle partial transfers.

ASCII/Latin-1 tables and ASCII display are enabled. HTTPS uses guest TLS 1.2/1.3
with certificate-chain, hostname/SAN and expiry checks against `/etc/ssl/cert.pem`.
`SSL_CERT_FILE` selects another guest CA file. Normal installers preserve an
existing trust bundle and install `/dev/urandom` and `/dev/crypto`. The browser
supports software crypto when the accelerator is unavailable.

UTF-8, JavaScript, compressed responses, cookies, external media and HTTPS proxy
tunnels remain outside this profile. A JavaScript verification page can be
rendered as HTML but its script cannot run. TLS failures abort the load without
an option to bypass certificate verification. Handshake waits are bounded at
60 seconds; body reads retain a 15-second timeout and poll keyboard cancellation.

Set `LYNX_RESOURCE_REPORT=1` in the **guest** when profiling. On normal exit the
browser prints `LYNX_HEAP_ARENA_CELLS`: growth of its guest allocator arena from
entry to exit. This allocator does not return its arena to brk, so the value
includes freed blocks and fragmentation and captures its high-water reservation.
It excludes static globals and is not a count of live allocations. Stack peak
is not instrumented. Acceptance runs record results in `build/runtime-*.json`.

The October 17 acceptance scenario (forms, a long request, a 150-row page,
oversize rejection and error recovery) recorded:

| Target | Static global cells | Heap arena high-water cells |
| --- | ---: | ---: |
| Native Fuse-X | 231,149 | 226,976 |
| RVM/FUZIX | 231,107 | 226,971 |

Each cell occupies two physical octets. The heap figures are about 443 KiB per
target and describe this workload, not a worst-case bound for arbitrary HTML.

## Network and UI acceptance

```sh
python3 ports/lynx/test_browser.py
python3 ports/lynx/test_browser.py --target ripple
python3 ports/lynx/test_install.py
python3 ports/lynx/test_install.py --target ripple
python3 ports/fuzix/tools/test_lynx_apps.py
python3 ports/fuzix/tools/test_lynx_apps.py --target ripple
```

The Ethernet peer validates guest DNS/IPv4/TCP checksums and complete HTTP
requests, supplies split headers and chunk framing, and checks GET text/select
encoding. Tests drive actual console keys and assert the current ANSI screen:
relative navigation/back, a long URL larger than the TCP transmit buffer,
scrolling, resize/reflow, slow-load cancellation, response limit, errors and
recovery, clean exit, plus a Telnet browser and independent second shell.
Installation tests cover both lock conventions, repeat installs, configuration
preservation, installed startup and a missing terminal database.
`test_lynx_apps.py` exercises the launchers' normal image builders with fresh and
existing disks, checking executable contents, data files, configuration
preservation, no-op updates and an actual installed browser launch.

For an opt-in Internet test through the existing macOS shared vmnet helper:

```sh
python3 ports/lynx/test_live.py --target native --address 192.168.64.248
python3 ports/lynx/test_live.py --target ripple --address 192.168.64.247
```

Use an address distinct from running guests. This performs guest DNS/HTTP to
example.com and Wiby, searches for `retro`, fetches an HTTP result and drives
Wiby's actual GET form. The helper uses its normal sudo authorization; no host
HTTP proxy or TLS gateway is involved. Site content is an external dependency.
Rapidly recreating guests with the same address can encounter stale vmnet NAT
state; use a fresh unused address when diagnosing this, instead of treating an
external-site timeout as a deterministic browser regression. Logs are retained
in `build/browser-*`, `build/live-*` and `build/install-*`.

## Reproduce the compile gate

Build `rcc` and `rcpp` in the root workspace, then:

```sh
python3 ports/lynx/probe.py --all
```

A C host compiler is needed only to build upstream's character-table generator,
`makeuctb`; set `HOST_CC`/`HOST_CFLAGS` if necessary. The browser itself uses
RCC's preprocessor and C16 frontend, guest libc headers, and the reviewed
`config/lynx_cfg.h`. No host configure results, headers, ncurses, socket library
or C ABI are substituted. `SOURCE_DATE_EPOCH` fixes the generated build timestamp.

The script verifies and extracts the vendored archive, generates checked source
adaptations and charset headers, then retains `.i`, `.ir` and diagnostic `.log`
files in ignored `build/probe/`. `results.json` summarizes the selected units;
any failed compilation makes the script fail. Specific source stems can be
passed instead of `--all`; `-j` controls compiler concurrency.

The configuration omits compression, most optional protocols,
localization and option menus. Only ASCII and ISO Latin-1 conversion tables are
enabled. Compilation is not evidence of runtime correctness, full C16 auditing,
complete charset coverage or arbitrary-page safety. Browser-specific transport
and UI behavior have the acceptance tests below; stack high-water measurement
and a broader adversarial renderer audit remain open.

## Recorded adaptations and compiler findings

`adapt.py` checks exact source fragments against the pinned archive and writes
adapted copies, leaving `upstream/` pristine:

- Replace GridText's private bitfields with explicit unsigned fields, preserving
  its four-cell pool allocation alignment.
- Widen SGML `TagClass` to unsigned long: its masks exceed 16 bits.
- Spell one known array size explicitly and derive two declarator bounds by
  counting the actual preprocessed target initializers, never host `sizeof`.

Open RCC regressions are registered in `c-test/tests/bugs/` for bitfields,
`sizeof(array_object)` in declarators, parenthesized array typedefs, and missing
`__DATE__`/`__TIME__`. The build supplies its own timestamp for the latter.

The macro failures discovered here were fixed in RCC: empty arguments, builtin
macro definedness, and token-based replacement/rescanning. `#`/`##` retain raw
arguments, ordinary uses prescan lazily, recursive names remain disabled, and
hash-map iteration no longer controls expansion. There is no external browser
preprocessing workaround. Linking also exposed folded-Phi ordering, register
exhaustion on long argument lists, and first-bank assumptions on global struct
pointers. These have focused executable regressions that were confirmed failing
before the fixes. Void conditional expressions now emit control flow without
invalid void allocations/stores; the focused native regression was confirmed
failing before the frontend fix. Banked globals preserve the existing two-cell pointer ABI;
small programs keep their prior fast addressing. Native/RVM guest probes cover
initializers, wide values, heap separation, cross-bank syscalls and fork/exec;
the RVM probe also passes with bulk loading disabled.

The shared [guest curses port](../fuzix/CURSES.md) has separate actual native/RVM
validation. Its scanner intentionally supports integer/string conversions only;
Lynx links the shared floating formatter and math library explicitly. Portable
`bsearch`, guest identity helpers and assertion output now live in shared libc;
`LYatoll` maps to the target's 32-bit `atol`, matching `off_t`, and Lynx uses the
existing guest `putenv`. Floating input scans and broader browser options still need review. The port
also checks terminal initialization failures instead of dereferencing a NULL
screen. Native console paste backpressure has a separate regression in
`fuse-x/test_console_paste.py`.

## Source and license

- Upstream: [Lynx](https://lynx.invisible-island.net/)
- Pinned release: 2.9.2
- Archive: [lynx2.9.2.tar.gz](https://invisible-island.net/archives/lynx/tarballs/lynx2.9.2.tar.gz)
- SHA-256: `99f8f28f860094c533100d1cedf058c27fb242ce25e991e2d5f30ece4457a3bf`

The complete unmodified archive is retained in `vendor/`. Extraction preserves
its notices and `COPYING` (GNU GPL version 2); individual libwww/source files
retain their own notices and terms. Local adaptations are build-generated from
`adapt.py`. This is a pinned source choice, not a claim about the latest release.

## HTTPS acceptance

Build the matching kernel and browser, then run the independent TLS fixture:

```sh
python3 ports/tls/tests/test_lynx.py --target native --executable fuse-x/build/apps/lynx.fxe
python3 ports/tls/tests/test_lynx.py --target ripple --executable build/fuzix/lynx.rxe
```

These use disposable disks. The fixture covers TLS 1.2/1.3, ECC and RSA servers,
chunked pages, redirects, expired/untrusted/mismatched certificates, interactive
HTTPS links, cancelling a stalled transfer, and returning to the shell.
`--smoke` checks one rendered HTTPS page; `--ui-only` checks the interactive
lifecycle. The plain HTTP/browser regression remains `test_browser.py`.
