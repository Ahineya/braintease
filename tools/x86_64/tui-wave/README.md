# Native x64 terminal application wave

These opt-in helpers prepare the existing mux, Commander and Rift sources as
static `riplix-x64-c16` RXE1 applications. They do not change the default app
selection, publish the mini catalog, install onto the HP, or edit the shared
RFL/compiler/curses sources.

```sh
python3 tools/x86_64/tui-wave/build.py mux rfl-fm rift
python3 tools/x86_64/tui-wave/package.py
python3 tools/x86_64/tui-wave/check_packages.py
python3 tools/x86_64/tui-wave/check.py --managed --boot build/x86_64-terminal-wave32/normal
```

The private cache and artifacts default to `build/x86_64-tui-wave`. No kernel
or disk build is required for compilation. The RFL inputs use the existing
native frontend, OS adapters and shared guest curses implementation; C inputs
use the same static bounded heap profile and freestanding linker as other
x64 apps. `RCC_NATIVE_MEMORY=checked` is selected explicitly. The ordinary
C16 ABI and RXE1 header remain in use.

Rift needs the separately reviewed 32 MiB **total** RXE1 package limit in the
linker and native loader. Its candidate is about 23.5 MB including metadata.
It will not run with the earlier 16 MiB loader. Commander and mux fit the
earlier limit. App compilation alone is not runtime acceptance.

The package subset reuses `tools/packages/catalog.py`, the portable package
serializer, and existing resource producers. Commander is package `rfl-fm`
with command alias `mc`; Rift provides `vi`. The corresponding data, Unicode
tables and license packages are included. Per existing catalog policy, aliases
duplicate the executable payload in their archive; the Commander and Rift
archives are therefore roughly twice their executable sizes. Configuration
preservation and dependency ownership use the package engine's normal rules.

An independently accepted Levee RXE1 placed at `apps/levee.fxe` is also
packaged. Its current keyboard manual override is copied to
`/usr/share/doc/levee/levee.1`; canonical `/usr/man` pages retain their shared
`man-pages` ownership. This optional ninth package does not alter the eight
TUI/resource package identities or the immutable earlier QEMU receipts.

`check_packages.py` installs the complete subset into a disposable host root,
checks exact executable and alias hashes and existing user configuration, then
uninstalls it. It does not execute x64 binaries on the Mac.

`check.py` clones the accepted seed into a disposable sparse filesystem and
places it inside a fake NVMe/GPT fixture with captured geometry. It drives
real QMP/i8042/TTY input and exercises the shared curses decoder through
`keycheck`, Commander navigation/F10 quit, mux PTY capacity/tabs/child shells/
detach/list/kill, and Rift insertion/Escape/save/quit when built. Small
screenshots and receipts are retained; temporary disk images are removed.
The `keycheck` source belongs to the platform owner and its RXE1 must first
be built into this helper's `apps` directory using `cc.py --curses --static
--heap-profile bounded --linker tools/x86_64/app_link.py`.

Use `--packages` for a slower full guest package installation before launching
the applications from managed `/usr` paths. A successful host transaction
does not claim guest installation speed. `--managed` installs those same
archives with the portable host engine, preserves its bounded relative package
symlinks in the private C16 fixture, checks the guest package database, and
executes the managed paths. This avoids measuring large archive extraction
through TCG while still verifying actual RXE1 execution and package layout.
QEMU receipts are separated by test
mode and exact EFI/application hashes; an existing successful receipt is never
overwritten. This suite does not certify physical Fn/media keys, pointing
devices, AX211 traffic, audio, Commander SFTP mounts or complete terminal
emulation.

The current general x64 builder deliberately excludes mux pending PTY
acceptance and treats RFL apps separately. To integrate this wave into the
named package updater, its owner should dispatch these three names to this
builder instead of rebuilding the offline C collection, retain the existing
`mc`/`vi` aliases and data groups, and use a compatible accepted EFI before
publishing Rift. No default whole-catalog behavior was changed here.

The current nine-package subset was subsequently published to the mini and
installed through the actual HP package manager. All executable/alias hashes
matched; Levee arrow/edit/save, Commander F10 exit and Rift edit/save passed
through SSH PTYs. Package versions and Rift's exact image persisted across
the unattended Ubuntu/Riplix round trip. These checks do not certify the
physical framebuffer cursor or local full-screen display. Evidence:
[`physical-acceptance.json`](../../../build/x86_64-terminal-wave32/physical-acceptance.json).
Use the [HP install checklist](hp-install-plan.txt) for the named package order
and bounded entry probes rather than installing the entire app collection.
