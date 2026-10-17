# Hosted application coverage

The existing Ripple FUZIX application list is the source of the build inventory.
`apps.py` reads that list and provides native recipes for the utility, V7, MWC,
editor, Tcl, awk, game, and network sources. It never generates files in
`ports/fuzix/`.

The 2026-10-17 inventory builds **113/113 programs and aliases**, including the
Bourne shell, Vile, Levee, awk, make, m4, dc, Picol Tcl, and the ported games.
Fifty-two application behavior checks cover file operations, text tools,
programming tools and system reports. Compilation alone is not a promise that
every interactive mode or option works.

```sh
make -C fuse-x applications
python3 fuse-x/apps.py sh levee awk
python3 fuse-x/test_applications.py --executable fuse-x/build/fuse-x
python3 fuse-x/test_shell.py --executable fuse-x/build/fuse-x --compiler target/release/rcc
```

Build results, sizes, requirements and diagnostics live in
`build/application-report.json`, with individual logs under
`build/application-logs/`. Successful and blocked results are cached against
compiler, source, header and recipe inputs. Named builds fail on errors;
`--all` completes the inventory and records blockers. A failed rebuild removes
the old output from the install set. Alias packages can reuse an identical
canonical build.

Normal launches install the successfully built local applications and support
files, including terminal definitions, accounts, manuals, make's default rules
and Sokoban levels. They preserve user files and expand old bootstrap roots to
65,535 blocks (128 MiB minus 2 KiB) with at least 1,024 inodes, saving the exact
previous image before replacement.
The build report determines managed programs; stray packages in `build/apps/`
are not silently installed.

The default `/bin/sh` is the existing Bourne shell. `/bin/fxsh` retains the
bootstrap shell for debugging. One generated copy of the Bourne shell's main
source replaces its V7 null-pointer decrement sentinel with an explicit pointer
constant. All imported sources remain intact. Custom signal handlers and
nonlocal jumps use native continuations, while FUZIX owns pipes, process groups,
wait/stop/continue behavior and the four guest PTY pairs.

Levee's native build also adapts its save routine to create temporary files in
the destination directory. That keeps replacement atomic when editing a file
on `/mnt/host`, where renaming a temporary file from the root image's `/tmp`
would correctly return `EXDEV`. The `vi` alias uses the same package.

The ten network programs install normally. The hosted kernel enables
`CONFIG_NET` and reuses the existing socket bridge and guest protocol stack.
`network.py` generates a build-only adaptation replacing the bridge's fixed
Ripple bank with an allocation in shared hosted kernel memory. Network polls
run during idle and syscall entry/return. See [networking](README.md#networking-and-remote-shells)
for launch settings and [the network tests](test_network.py) for runtime coverage.

Current limits still include cooperative scheduling, 16 process slots, up to
sixteen 64,000-cell global banks per program, a bounded heap window, and
provisional RAM accounting. Native packages may reach 64 MiB and stream through a 4,096-cell
kernel buffer. Native code remains trusted host code; compatibility pointer
checks are not host isolation.
