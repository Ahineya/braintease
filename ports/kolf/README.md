# Kolf for Riplix and Fuse-X

A C/Nano-X reimplementation of KDE Kolf with all eight original courses (128
holes). The executable embeds compressed courses, so a copied binary needs no
data directory. Practice is the default; press **C** to choose another course.
The portable course loader and simulation have no Nano-X dependencies. Both targets use the same frontend and engine.

Start the graphical environment with `startx`, then run `kolf &` in a terminal.
On Fuse-X the shared-folder build can run as `/mnt/host/kolf &`.

| Control | Action |
| --- | --- |
| Mouse movement | Aim **towards** the pointer, including with the ball against a border |
| Hold left mouse / Space / Down | Pull the putter back; release to swing |
| Left / Right | Rotate by 2 degrees; Shift: 6 degrees; Ctrl: half a degree |
| A | Toggle classic / advanced three-click putting |
| Advanced: three presses | Start charging, select strength, select accuracy |
| Escape | Cancel a pending swing without spending a stroke |
| G | Toggle the aiming guideline |
| Enter | Next hole after completion/stroke limit; new round after the last hole |
| U / Ctrl-Z | Undo the last shot, including its penalty and moving-object state |
| Water dialog | Enter accepts the default drop; arrows/Tab select; D drops, R rehits; U undoes |
| R / N | Retry the current hole / restart the round |
| P | Pause or resume |
| C | Choose one of the eight original courses |
| F1 / ? | Help and About |
| Q | Quit |

Classic putting is the default. The club backs away on a 20 ms timer, adding
1.5 internal strength units each tick. Release starts the visible forward swing;
the ball launches when the club reaches it. Holding to the maximum starts the
forward swing automatically (55.5 / 8 = 6.9375 launch speed). Mouse distance
does not set power. Aiming towards the cursor intentionally improves on upstream
Kolf's behind-the-ball aiming: an inward shot is possible even at the border. Fast taps retain upstream's zero-strength fallback speed.
The initial club orientation matches upstream, rather than aiming at the cup.

Advanced putting (`A`, or start with `--advanced`) uses Kolf's nonlinear charging
curve and circular blue-to-red strength indicator. The second press fixes power
and starts the return sweep; the third determines accuracy against the white
reference line at the bottom. Poor timing changes direction and can lose power;
missing the accuracy window finishes a bad stroke automatically. The ring and
club are animated while the ball remains stationary. Strength is divided by 8
at launch, including advanced shots above speed 8. Key repeats and an unrelated
button release cannot select phases or accidentally launch another shot.

`putting.c` owns the timestamped input/animation model independently of painting.
Ordinary delayed frames do not change power accumulation. After a long display
stall the frontend bounds timer catch-up to 80 ms, so rendering cannot consume
a whole input window; upstream Qt timers also coalesce overdue callbacks.
Drawing stays capped at 30 fps;
the dial uses precomputed integer directions and scanline sectors, submitted as
one compact bitmap per frame. Escape,
retry, changing mode/course, help, pause or loss of focus cancels a pending
swing. Release after cancellation cannot fire. Moving obstacles keep running
while preparing a shot unless paused or showing help/the course selector.

Each shot costs one stroke. Water adds one penalty and opens a modal recovery
choice, freezing the ball and moving obstacles. **Drop Outside of Hazard** is
the default: the ball backs out along its incoming direction in three-pixel
steps until its whole radius clears visible water. **Rehit From Last Location**
returns only the ball to the pre-shot location, leaving obstacles where they
stopped, as upstream does. Arrow keys or Tab select; Enter confirms, D/R choose
directly, and clicking either option works. Escape keeps the decision pending.
If no dry position exists along the drop path, the dialog offers rehit without
charging another penalty. Zero-speed entries (for example, blinking water) use
a bounded radial search rather than normalizing a zero vector.

**U / Ctrl-Z** or the Undo button restores the last pre-shot checkpoint: ball,
score, penalties, platforms and carried objects, gate position/direction, blink
phase, warp state and simulation clock. It also works while rolling, in a warp,
in the water dialog, or after sinking the ball/reaching the stroke limit, until
you advance to another hole. It is one reusable checkpoint, like upstream, not
a stack of earlier shots. A new shot replaces it; retry, new round and course
changes clear it. Undo cancels any prepared swing and restores its aiming angle.

Leaving the green restores the checkpoint including moving objects, with only
the played stroke charged, matching upstream's score rule. Holing out or reaching the course's
stroke limit enables the next hole. **Kolf stores 4 as its unlimited-strokes
sentinel.** A genuine limit displays **STROKE LIMIT REACHED**, separately from
**BALL HOLED!**. Scores are per-round; there is no persistent leaderboard or
multiplayer.

## Course compatibility

`kolf path/to/course.kolf` loads a course. `kolf --check-course [PATH]` validates
one without opening the display. With no path, both use embedded Practice.
The default course is also installed at `/usr/share/kolf/courses/Practice.kolf`.

All eleven upstream object types are supported: walls, cups, sand, puddles,
slopes (all five gradients), bumpers, black holes, bridges, floating platforms,
windmills and signs. This includes:

- Elevated bridge/platform surfaces shielding the ball from ground hazards;
  slopes can remain on the ground or be carried on a platform.
- Moving platforms carrying the ball and supported cups/obstacles; reflected
  endpoint motion, configurable speed and independently enabled side walls.
- Windmill stubs and moving guards on either edge, including contact with a
  stationary ball.
- Bumpers with Kolf's speed boost and declining per-shot bounce ceiling.
- Black-hole capture speed, distance-based transit, directional ejection,
  fractional exit speeds and per-hole-object loop limits per shot.
- Blinking sand/puddles and signs with untranslated KConfig comments (basic
  HTML is stripped for display). Locale alternatives are accepted.

`kolf --list-courses` lists built-in identifiers, and `kolf Hard` starts that
course directly. Original files are installed under `/usr/share/kolf/courses/`.
The built-in copies omit locale alternatives before DEFLATE compression; the
installed upstream files remain unmodified. Only the selected course is
inflated and parsed. The shared bounded DEFLATE library (also used for PNG) is
reused rather than adding another decompressor.

Limits are 36 contiguous holes, 256 objects per hole, 256 Ki input cells,
1024 cells per line and 16384 INI assignments. Unknown objects/properties,
invalid numbers and missing starts/cups fail before committing a course.
Legacy `hasFinalLoad` is accepted; supports are resolved after loading all
objects. Degenerate saved wall segments have no collision surface.

All eight pinned upstream courses load and every hole has undergone automated
simulation. **This is object/format compatibility, not exact physics parity.**
The original modified Box2D solver, contact ordering and bumper direction
jitter are not reproduced: our bumper rebound is deterministic, and collision
trajectories can differ. Sign text uses simple plain-text layout rather than
Qt's full HTML renderer. Sound, multiplayer and a level editor are outside this
milestone. Golden upstream shot traces remain useful future compatibility work.

## Physics and rendering

`engine.c` uses a fixed 11 ms logical step and swept ball/segment collisions,
including wall endpoints. Contact resolution is bounded to four contacts per
tick. Terrain friction, slope forces and cup speed thresholds are adapted from
Kolf; moving contacts use relative wall velocity. The collision engine replaces
its modified C++ Box2D and Qt overlap tests.
Results are not trajectory-identical to upstream. Exact solver compatibility
would require reference traces and potentially adapting more of Box2D.

The engine uses binary64 arithmetic and the guest's accelerated math functions.
It allocates nothing during simulation. Live object state belongs to the game,
so retry resets platform motion, blinking and warp state without altering the
loaded course. A fixed-capacity pre-shot snapshot copies only the live hole
objects when a shot begins. Restore advances the rendering revision even though
the simulation clock rewinds, so cached terrain cannot hide a restored object. Prepared integer bounds reject distant objects before detailed collisions, and constant slope forces are cached.
Physics advances independently of the display, with bounded catch-up after stalls. The frontend caps animation at
about 30 frames per second, uses integer ellipse/slope spans and cached aim
vectors, caches ground and composed terrain in two 400-by-400 server pixmaps,
restores ball/aim damage rectangles on static holes, and recomposes animated
obstacles only when their visual state changes. Static holes block on events
while idle; animated holes continue moving before a shot. Help and pause freeze simulation. The initial artwork is drawn procedurally; no SVG
rasterizer, Qt runtime, image assets or sound effects are required.

## Build and verify

```sh
python3 fuse-x/graphics.py --only kolf
make -C ports/fuzix BUILD="$PWD/build/fuzix" kolf -j4
python3 ports/kolf/assets.py --check
```

Outputs are `fuse-x/build/apps/kolf.fxe` and `build/fuzix/kolf.rxe`. Both builds
embed their guest libc so the copied game also works on images predating shared
library support.
Normal graphics/userspace image builds include the game, original courses,
license and provenance. Existing disk images are not modified by these commands.

`tests/engine.c` checks flat/sand stopping distances, straight and diagonal
walls, endpoint contacts, fast wall crossings, bounded/unbounded edges, water,
cup speed limits, slope geometry, stroke limits and malformed course handling.
Compile it with `engine.c`, `putting.c`, `course.c`, `libraries/ini/ini.c` and libm for host
checks. Build the guest probes and run real kernels with:

```sh
python3 fuse-x/cc.py ports/kolf/tests/engine.c ports/kolf/engine.c \
  ports/kolf/course.c ports/kolf/putting.c libraries/ini/ini.c --float-format --static \
  -o fuse-x/build/apps/kolfcheck.fxe
make -C ports/fuzix BUILD="$PWD/build/fuzix" "$PWD/build/fuzix/kolfcheck.rxe"
python3 ports/kolf/tests/guests.py --target both
```

The runner uses disposable disks and saves captures/logs in `build/kolf-tests`.
It exercises the original course, keyboard/mouse hold/release controls,
backswing cancellation, three-click putting, help, pause, a
complete nine-hole fixture round, unlimited/limited UI states, course selection,
animated obstacles, cursor aiming at the border, both water choices, modal
freeze, undo via U/Ctrl-Z (including a holed ball), restart, idle drawing and cleanup. The RVM
path uses the JIT. `kolf --benchmark` executes 9000 physics steps across Practice
and reports elapsed guest-clock milliseconds; it excludes rendering and pacing.
`kolf --benchmark 100` runs a shorter 900-step probe.
The native round fixture uses a short released swing; the RVM fixture holds to
the automatic maximum and sinks after a wall bounce, keeping its launch speed
independent of host scheduling. The standalone putting tests cover release
timing separately.

## Upstream and license

Source: <https://invent.kde.org/games/kolf>, revision
`ce6379940d60ef11f7587e4162fd4d1f7439e9a0`. `source.json` pins all eight
unmodified courses and their SHA-256 values; `assets.py` verifies them and regenerates
the compressed, octet-safe `courses.inc`. Course authors remain credited in the
original metadata. The adapted rules credit Jason Katz-Brown and Stefan Majewsky in `engine.c`.

This port is GPL-2.0-or-later; see `COPYING`. No Box2D code, KDE artwork or sound
files are bundled. This is an independent port, not an official KDE release.

`tests/sweep.c` is a host-only adversarial run through all 128 holes, checking
that deterministic shot sequences remain finite and bounded. Compile it with
the same engine/loader sources and libm; pass `ports/kolf/courses/*.kolf`.

## Fidelity audit (pinned upstream revision)

The source and manual were compared again when implementing proper putting.
Loading a course or running finite physics does **not** establish identical
playability or shot outcomes. These are the remaining distinctions:

| Area | Status / difference |
| --- | --- |
| Normal putting | Restored: animated hold/release backswing, 20 ms charging, automatic maximum swing, launch at contact, Escape cancellation. |
| Advanced putting | Restored: nonlinear power, circular gauge, three presses, accuracy offset and bad-stroke random loss. The charge-down formula intentionally fixes upstream's stall near strength 55.1 (`pow(2,s/65)-1.8` tends to zero). |
| Aiming | **Intentional improvement:** aim towards the cursor so shots at the border remain possible. The 2/6/0.5-degree keyboard increments, initial leftward shot and optional guideline remain. Pointer position is sampled on motion; stationary-cursor tracking on a moving platform is still absent. |
| Power limits | Corrected: engine accepts upstream's advanced power above 8; the normal maximum is approximately 6.94. |
| Stroke limits | Corrected: 4 means unlimited; reaching a real limit is distinct from sinking the ball. |
| Out of bounds | Restores the pre-shot ball and complete moving-object checkpoint, charging the played stroke only. The centre-outside-0..400 rule remains. |
| Water recovery | Implemented: default Drop Outside of Hazard or Rehit From Last Location, with one +1 penalty. Drop walks backwards in 3px steps and clears the entire ball; rehit moves the ball only. The modal freezes animation. Zero velocity and impossible drops use bounded handling rather than upstream's unbounded loop. |
| Undo | Implemented: one reusable pre-shot checkpoint with ball, scores and complete live object/animation/warp state. This also restores animation phase/direction, beyond upstream's object-position-only snapshot. U, Ctrl-Z and a sidebar button work; advancing/retrying/changing course clears history. |
| Hole/round progression | Ours shows a per-hole summary and waits for Enter. Upstream advances automatically and has a richer scorecard. Intentional current UI difference. |
| Physics/terrain order | New bounded swept-circle solver instead of modified Box2D. Contact ordering, friction/terrain update ordering and strut overlap tests differ. Establish upstream golden shot traces before claiming trajectory parity. |
| Bumpers/layers | Same bounce-strength rules, but engine bumper direction is deterministic (upstream jitters by -1/0/+1 degree). Support selection currently approximates shape overlap. |
| Slopes/course numbers | Stock courses load. Ours restricts slope grades and dimensions to bounded integers; upstream accepts some real-valued settings. This is not unrestricted third-party course compatibility. |
| Course hints | Upstream's Up-key information overlay is absent; ours currently draws slope arrows and warp exits permanently. |
| Presentation | Procedural artwork, plain-text sign rendering, no sound, and no persisted putting preferences. Upstream uses SVG, rich-text signs, audio and saved settings. |
| Timer stalls / cancellation | The pure model uses timestamped deadlines; the frontend bounds overdue catch-up to 80 ms after display stalls (Qt coalesces overdue timer callbacks). Ours cancels pending swings on focus loss/pause/modal entry to prevent stuck input; upstream can pause and resume its putter timer. |
| Multiplayer/editor | Explicitly outside this port's scope. |

Snapshot/undo and water recovery are implemented. Exact physics parity is
deferred; the next prerequisite is upstream reference traces for contacts and
terrain before deciding how much of Box2D to port.
The GUI-independent putting tests already verify timing invariance, maximum
strength, cancellation, accuracy windows, automatic timeout and clock rollover.
