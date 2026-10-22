# LVGL on Fuse-X

LVGL 9.6.0 runs as guest C16 code with a small GX window adapter. `lvcalc`
is a calculator; `lvselect` is a file selector; `lvgallery` demonstrates the shared
Graphite controls, typography, lists, tables, menus and dialogs. `lvdata` is a
CSV/TSV table-and-plot workbench that preserves its source files. `lvedit` is a
tabbed graphical text editor sharing Rift's C text engine. All five use GX
directly; no Nano-X server or `startx` is needed. Fuse-X can present them as
native windows or inside its optional single-window desktop.

The guest `fusedeshell` adds the first FuseDE shell in nested desktop mode:
an application menu, pinned/grouped dock and separate Riplix/clock panels.
The normal `scripts/run-fuse-x.sh --desktop` build installs all six executables
and the curated system catalog, then starts the shell through `/init`. Native
host windows and x64 boot behavior remain unchanged.

Build and stage for host-share testing from the repository root:

```sh
python3 ports/lvgl/build.py --fetch --stage fuse-x-shared
```

Then run in Fuse-X:

```sh
/mnt/host/lvcalc.fxe
/mnt/host/lvselect.fxe /mnt/host
/mnt/host/lvgallery.fxe
/mnt/host/lvdata.fxe /mnt/host/lvgl/examples/measurements.csv
/mnt/host/lvedit.fxe /mnt/host/lvgl/examples/editor-demo.c
```

The builder only writes build outputs and the explicitly selected staging
folder. It never opens or installs into a normal OS image. Omit `--fetch`
once the verified archive is cached. `--app all` builds the five ordinary apps;
use `--app fusedeshell` separately for the desktop shell. Use `--app lvcalc`,
`--app lvselect`, `--app lvgallery`, `--app lvdata`, or `--app lvedit` to
build just one application. The executables are static, so they need no LVGL
or libc library installation in the guest.

Applications declare their stable desktop identity with
`gx_lv_application(app_id, name, icon)` before `gx_lv_open()`. The adapter copies
the strings, probes the optional `GX_SESSION` registry, and registers the caller
before creating its window. This never changes the window title, claims manager
authority or associates child processes. Older kernels and registration failures
leave the interface usable. Icons reference the existing named Lucide assets.

| Executable | Application ID | Icon |
| --- | --- | --- |
| `lvcalc` | `org.riplix.Calculator` | `calculator` |
| `lvedit` | `org.riplix.TextEditor` | `file-code` |
| `lvgallery` | `org.riplix.GraphiteGallery` | `grid-2x2` |
| `lvdata` | `org.riplix.DataViewer` | `grid-2x2` |
| `lvselect` | `org.riplix.FilePicker` | `folder` |

`lvselect` is a standalone utility. The editor and data viewer's embedded file
dialogs stay in the existing application window and create no dock entry.
The graphical terminal similarly registers `org.riplix.Terminal` with the
`terminal` icon; its shell and subprocesses are not implicitly desktop apps.
The optional C registration helper also has an independent compatibility test:
`python3 ports/graphics/tests/test_application.py`.

## FuseDE guest shell

`shell.c` renders the dock and menus using shared Graphite components. Its
leftmost Applications button opens a compact category menu with word search,
centered above the dock. With search focused, Up/Down selects an available
result and scrolls it into view while typing continues to filter; Enter launches
the selection. Selection wraps at both ends, and held arrows repeat.
Query/category changes select the first available result, and
unavailable rows are skipped. Its fixed **Run…** action opens a content-sized
program-name/path dialog with quoted arguments and an optional **Run in terminal**
checkbox. Status/error space appears only when needed.
Known catalog programs retain their GUI/TUI policy; other programs resolve
through `PATH` or an explicit path. The parser does not evaluate shell syntax.
Run/Enter submits, Cancel/Escape dismisses, and exec errors remain in the dialog.
The top-left Riplix pill opens Applications, About FuseDE and Exit desktop shell;
the disconnected top-right clock uses `Wed 21 Oct 22:43` in guest local time.
An idle pinned launcher starts the app, a single running window activates, and
multiple windows open a chooser. Context menus add Open new, pin/unpin and
cooperative group show/minimize/close. Apps retain independent GX surfaces and
processes; the shell does not embed their widget trees.

`shell-catalog.c` reads `/usr/share/applications/*.ini`, with
`FUSEDE_APPLICATIONS` for an alternate directory. Entries declare stable IDs,
names, icons, categories, GUI/terminal kinds and exact executable/argument
vectors. The installed path is preferred, followed by one explicit host-share
fallback. Missing programs are marked unavailable. No shell evaluation or
implicit executable guessing is used. The
[catalog schema and parser/launcher limits](../fusede/README.md#graphite-application-catalog)
describe validation and the checked-in 13-entry collection.

Terminal entries launch through `gterm --app-id ID --app-name NAME --app-icon ICON`.
Commander, Rift, Music, Telegram, Chat, Multiplexer and JavaScript therefore get
their own stable app identities and initial window titles. Multiple instances
share one dock icon, while their child shells and windowless helpers add no
entries. Legacy Nano-X clients such as NetSurf and `nxview` remain usable as
earlier porting experiments; further catalog work is not a current goal.

Pins merge catalog defaults with `$HOME/.config/fusede/pins.ini`. Pin/unpin saves
an atomic user override; image installation never replaces user pins.
`FUSEDE_PINS` can select a separate preference file. The checked-in
`ports/fusede/pins.ini` is an empty-override example, not an installer payload.

The shell claims process-bound `GX_SESSION` manager authority and uses the
nested-only `GX_PANEL` capability. Its output-sized backing surface exposes
only bounded dock, top-pill and menu rectangles for composition/hit testing;
top/bottom work-area reservations remain separate. Client pixels and input pass
through the gaps. Menus temporarily request keyboard/modal handling. Hidden,
released or revoked panels stop reserving space or intercepting input; shell
exit restores compositor fallback controls without closing client applications.
Native mode and rescue startup retain graphical terminals. This is a regional
overlay profile with optional straight-alpha uploads for rounded panel edges,
not a general transparent-window or x64 adapter. Alpha panels use ARGB8888
LVGL strips and `GX_PANEL_UPLOAD`; ordinary apps keep the accelerated opaque
RGB888 path. BGRA channels remain logical octets in C16, and publication keeps
color and coverage together. The adapter also keeps LVGL's themed bottom
layer transparent, so it cannot flatten coverage before upload. Opaque panel
fills use bank-aware bulk copies; the alpha strips use LVGL's software renderer.
This avoids desktop-colored corner patches and antialiasing fringes over client
backgrounds.

For manual staging against an existing compatible kernel:

```sh
python3 ports/lvgl/build.py --app all --stage fuse-x-shared
python3 ports/lvgl/build.py --app fusedeshell --stage fuse-x-shared
mkdir -p fuse-x-shared/fusede/applications
cp ports/fusede/catalog/*.ini fuse-x-shared/fusede/applications/
scripts/run-fuse-x.sh --run-only --desktop
```

`/init` prefers `/bin/fusedeshell`, otherwise `/mnt/host/fusedeshell.fxe`, and
selects the installed catalog directory or `/mnt/host/fusede/applications`.
`--run-only` does not install or refresh either. An older kernel without panel
support retains its terminal/fallback path. Exact global keybindings,
screenshot tools, workspaces, tiling, fullscreen spaces and Activity
Monitor remain future work.

Independent checks use temporary fixtures or disposable offline images:

```sh
python3 ports/lvgl/test_shell_catalog.py
python3 fuse-x/test_graphite_integration.py
python3 fuse-x/test_graphite_shell.py
```

## Graphite shared UI

`ui.h` / `ui.c` own the theme, typography, control states and common components.
The GX adapter installs the theme; plain LVGL buttons, text areas, checkboxes
and containers receive consistent defaults automatically. The component helpers
add semantic roles for primary/tool/list buttons, text, panels, single-line
inputs, lists and palette swatches. Apps own their data and commands. They use
flex/grid layouts and logical measurements rather than repeating colour tables
or manually placing every child.

| Role / metric | Logical size | 2× backing size |
| --- | ---: | ---: |
| Secondary text | 12 | 24 |
| Body / controls | 14 | 28 |
| Heading / calculator expression | 16 | 32 |
| Control height | 30 | 60 |
| List row | 28 | 56 |
| Standard spacing / window inset | 8 / 12 | 16 / 24 |

The palette uses neutral graphite (`#191a1c` canvas, `#212225` panels), subtle
solid borders, bright text, readable secondary text and a restrained blue
accent. Ordinary application surfaces are opaque; the shell preserves alpha
at panel edges. There are no shadows, gradients or glass
effects. Hover, pressed, disabled, selected, input-error and keyboard-focus
states are shared. Focus outlines sit inside control bounds, avoiding clipping
in dense layouts. Carets only appear in focused inputs and do not blink; idle
apps stop publishing frames.

Clicking an empty panel or window surface releases the keyboard target and
clears text selection/carets. Tab then starts at the first enabled control;
Shift-Tab starts at the last. Control clicks and drags retain LVGL's normal
behaviour. The pointer policy clears group focus without changing traversal
order and respects frozen groups. Its pinned private access to LVGL 9.6's
group focus/editing fields is isolated in `ui.c`, alongside theme font roles.

GX reports physical key transitions. The shared adapter repeats navigation and
editing keys (arrows, Home/End, Page Up/Down, Backspace and Delete) after 500 ms,
then every 100 ms. Raw app filters receive repeated presses; the event-driven
LVGL keypad receives a fresh press for each repeat. Enter, Space, Escape, Tab
and modifiers do not repeat. Releasing the key, losing focus, changing the
focused widget/group, pointer presses, resize or input overflow cancels the
hold. Call `gx_lv_cancel_key_repeat()` before replacing a UI context, as the
shell does before deleting a menu. Text input keeps its existing host-layout
path; gterm and SDL retain their own repeat handling. Repetition adds no idle
polling when a key is not held.

The direction combines classic desktop control density and keyboard conventions,
Apple's emphasis on hierarchy, and Linear's restrained surfaces and alignment.
It does not reproduce another platform's assets or chrome. References:
[Microsoft command buttons](https://learn.microsoft.com/en-us/windows/win32/uxguide/ctrl-command-buttons),
[Apple Human Interface Guidelines](https://developer.apple.com/design/human-interface-guidelines),
[Linear's UI redesign](https://linear.app/now/how-we-redesigned-the-linear-ui).

`ui_stack` supports zero or standard (`UI_SPACE`) gaps. `ui_input_height` fits a
complete font line inside its border, preventing vertical caret-scroll jumps.
Shared styles and font roles retain their identity when density changes;
application resize callbacks update their remaining logical geometry. The
pinned LVGL 9.6 private dependency for theme font-role fields is isolated in
`ui.c`. Theme cleanup releases objects/styles before LVGL teardown.

List rows share the opaque parent background; ordinary rows add no redundant
fill or invisible border. Highlighted rows paint a flat selection/hover colour.
Before icons, the compact layout's 2× 30-frame workload measured a median 0.795 s across three
fresh-process runs (0.786–0.812 s), compared with 1.122 s before this row cleanup
(1.116–1.162 s). This is headless rendering throughput, excluding capture and
the native compositor; it does not promise an on-screen frame rate. The 33 ms
refresh interval is unchanged. The layout/window/font sizes differ from the
earlier measurements below. Reports: `build/lvgl/bench-graphite.json` and
`build/lvgl/bench-graphite-final.json`.
With Lucide folder/file rows, the same workload measured a median 0.997 s
(0.872–1.193 s), including the additional glyph widgets. Direct RGB uploads
remain enabled with no colour-conversion pass. See `build/lvgl/bench-lucide.json`.

`lvgallery` has Controls, Typography, Lists, Icons, Data, Workspace, Plots, Forms
and Editor pages. Click the sidebar or use F1–F9; Escape closes it. The controls page provides live actions, editable
Unicode text, native clipboard, a checkbox, disabled examples and explicit
state samples. The validation input is editable and checks whether its path
names an existing directory. Both input drafts and the checkbox survive page changes. Typography
shows Latin/Greek/Ukrainian text and palette roles; the list supports keyboard
selection and wheel scrolling. All samples use the same components as the apps.

### Shared actions and split panes

`ui-app.h` / `ui-app.c` provide application components above the basic controls.
An action has one ID, caption, icon, shortcut, handler and enabled state. Bound
buttons, generated menu entries and raw GX shortcuts all recheck that state.
Control/Command is modifier bit 0 and Shift is bit 1; both shortcut edges are
consumed, but only the down edge executes. Shared modal overlays block background
actions. Dynamic menus can combine `ui_actions_item` with entries such as recent
files. Action descriptions borrow their strings; action sets copy the descriptors
and own their button bindings. Deleting an action set detaches surviving buttons
and closes only a menu belonging to that set; deleting buttons first is safe too.

`ui_split` owns two persistent borderless content panes and a focusable separator.
It supports left/right and top/bottom layouts, pointer dragging, arrow-key
adjustment, Home/End, double-click to equalize, and hiding/restoring the second
pane. Minimums use logical points. The preferred ratio uses thousandths and
survives clamping in a smaller viewport; undersized panes share available space
without negative geometry. Resizing never reconstructs content. The root emits
`LV_EVENT_VALUE_CHANGED` for ratio/visibility changes, so applications can save
preferences. Call `ui_split_density` from the app's resize/density callback;
deleting the root releases the component.

### Shared forms and plots

`ui-forms.h` / `ui-forms.c` add a choice control, validated file-path dialogs
and modeless bounded-work progress. Choices own copies of up to 65 labels,
covering all 64 CSV columns plus a row-number axis. Arrows and Home/End select
while the control is focused; opening its shared popup focuses the current
choice. Popup navigation commits on Enter/click, while Escape leaves the old
choice intact. Rebinding/deletion closes only that control's popup.

Open and Save use the shared Graphite file dialog, with directory browsing,
Up/Home/location navigation, hidden files and a virtual file list. Directory
scans run in bounded batches; navigation and cancellation release unfinished
work. Browsing preserves the application's working directory. The path field
also accepts a complete path, so keyboard workflows do not require list clicks.
Open dialogs require an existing regular file. Save dialogs validate the parent
directory and require a second Replace action for an existing regular file.
Paths are retained exactly and rejected if overlength. Application validation
can keep a dialog open with an inline error; cancellation is delivered during
Escape and window teardown too. The dialog validates intent; the application owns its actual
write, error handling and atomic replacement.

Progress reports actual incremental work, including optional at-most-once
cancellation and public Cancel-focus support. It never runs or simulates jobs.
Applications start/stop the component at job boundaries and return to the input
loop between bounded batches. Stopping restores a suitable visible, enabled
focus target before the progress panel is hidden; a previously deleted control
cannot remain a borrowed focus target. No upstream dropdown or progress-bar widget is
enabled just to provide these controls.

`plot.h` / `plot.c` provide an owned immutable numeric-series model, independent
of CSV, LVGL and Syma. Invalid/nonfinite values remain gaps with their original
source-row identities. A bounded hierarchy retains extrema and supplies at most
four representatives per logical plot pixel plus a small margin, keeping large
datasets bounded by the visible viewport. `ui-plot.h` / `ui-plot.c` render those
representatives as line/scatter series with axis labels, row selection,
fit-to-data, drag panning, arrow-key panning and +/- zoom. Home or double-click
fits the data. Wheel zoom is not exposed by the current GX event route.

The plot batches the visible curve into one antialiased A8 coverage image,
instead of asking LVGL to draw each diagonal's scanlines separately. The
portable `plot-raster.h` / `plot-raster.c` helpers clip before walking a line;
their work follows the clipped line length and bounded stroke width, rather
than its rectangular area. Scatter circles use the same image. Coverage is
combined with a maximum, so adjoining segments do not repeatedly blend and
darken their shared pixels. The mask is rebuilt once when the view, model,
mode, density or plot size changes, and remains alive across the refresh's
partial-buffer strips. Ordinary repaints reuse it.

On supported Fuse-X kernels, the adapter batches these primitives through
optional `GX_RASTER_A8` coverage generation. It accelerates generic lines and
circles in caller-owned RAM; CSV data, plot sampling, labels, selection and
window presentation remain in the application. Older kernels and
`LVGL_SOFTWARE_MASK=1` use the portable C helpers with the same coverage rules.

Mask storage follows the largest prepared physical viewport, with an upper
limit of 8,388,608 A8 cells per plot: 16 MiB of physical C16 storage. Typical
split-workbench plots use much less. The widget releases its mask on deletion;
allocation failure or an unsupported size retains ordinary LVGL line/circle
drawing. Tick captions are formatted once per changed view or geometry, and
labels outside the current redraw strip are skipped. Twelve bounded A8 text
runs batch the tick captions and axis names using their original font coverage.
They share a 262,144-cell budget (512 KiB of physical C16 storage); changed text
or font releases only its own run. Wrapping, unsupported fonts/glyphs,
overlapping ink and failed allocations retain ordinary label rendering.
Pointer dragging, keyboard panning, gallery plots and Data Viewer all use this
same component.

SVG export uses the same clipping/model geometry and escapes labels. It writes
a sibling temporary file and replaces the destination only on success;
incremental export cancellation removes the temporary file and preserves an
existing destination. PNG/raster export is a future addition.

The gallery's Plots page contains a real 1,000-sample series with an isolated
peak and missing-data gap, line/scatter choices and empty/non-numeric states.
Forms exercises 65 choices, Open/Save validation, replacement confirmation and
a cancellable rolling checksum over generated samples. The gallery never writes
to a path selected through its Save demonstration. Page changes stop active
jobs and release plots only after unbinding their borrowed models.

The gallery's Workspace page demonstrates duplicate Run controls, a shared menu
and shortcuts, a live enable checkbox, and nested splitters in both axes. Its
example result is presentation sample data, not an embedded Syma evaluator.
Ctrl+R runs the sample, Ctrl+Backspace resets it, Ctrl+D toggles the preview and
Ctrl+I opens an informational modal.

Data Viewer uses the same action set for toolbar/menu/shortcut commands. Ctrl+D
or View → Details pane opens a wrapped selected-cell preview alongside the table.
Drag or keyboard-adjust the divider. Visibility and the preferred divider ratio
are saved with other options in `$HOME/.lvdata`; existing preferences continue
to work. A preview is limited explicitly to 4 KiB; copying retains the existing
32 KiB clipboard limit. The source file remains read-only.

The [Graphite roadmap](../../docs/graphics/graphite-roadmap.md) records the
workbench milestone and next steps: document/editor components,
graphical Syma worksheets, packaging and desktop services. The ongoing
[SDL software port](../sdl/README.md) remains an independent GX client.

### Tables, menus and dialogs

`ui-data.h` / `ui-data.c` provide a shared two-axis table, popup menu and modal
dialog components. The gallery's Data page (F5) demonstrates all three with a
100,000-row generated dataset, sortable headers, draggable column edges,
keyboard navigation, an Actions menu and a validated Table options dialog.
Its Standard/Compact column choices demonstrate the shared radio list.

The table requests text from an application callback only for visible cells.
A bounded ring of at most 40 rows by 32 columns caches single-line UTF-8 previews
and their fitted widths. The body paints these with LVGL text/rectangle drawing
primitives; it does not create cell or row-number widgets. Only column headings
are widgets. Scrolling translates the header through LVGL scroll coordinates;
it does not change header layout properties for each horizontal pixel. Headers
that remain visible keep their widget identity, and only incoming columns are
rebound at a column boundary. The row-number corner floats above the scrolled
headings. A vertical scroll does not rebind or relayout the header, and
unchanged cached records are reused across fractional scroll movements. Cached
glyph bounds also skip text hidden behind the gutter, even when the wide blank
part of its cell remains visible. Display density changes preserve logical
scroll offsets. A bounded A8 text-run cache batches each supported cell and
row number into one draw operation. It reuses the font's original coverage
samples and recolours them at draw time, so selection does not duplicate masks.
Nonzero glyph overlaps retain ordinary label drawing to preserve exact blend
rounding. Unsupported fonts, exhausted cache budgets and failed allocations also
use the label path. The limit is 2,097,152 logical A8 samples (4 MiB of C16 host
storage) per table. Only offscreen masks may be evicted while drawing; visible
runs remain pinned across partial-buffer strips. Refresh, density changes,
ring replacement and deletion release the corresponding masks. Resizing one
column refits its text without throwing away unchanged columns.
Document size does not determine widget count or the cost of a viewport redraw. Column headings
and row numbers remain visible. Columns range from 64 to 480 logical points;
the current profile supports 64 columns and windows up to the hosted graphics limit of 1920×1080 logical points.
Use `ui_table_bind` for dimensions, `ui_table_heading` for names,
`ui_table_callbacks` for selection/sorting and `ui_table_select` for navigation.
The application owns data, ordering and sort semantics. Call
`ui_table_refresh` when callback values change, and `ui_table_density` from its
resize callback; root deletion releases the table and its text cache.

Menus and dialogs temporarily own a separate keyboard focus group. Tab stays
inside the overlay; Escape dismisses it. Menu arrows, Home/End and Enter work
with the keyboard; clicking outside a menu dismisses it. A menu closes before
its command callback runs, so that command can open a dialog. Dialog action
callbacks may return zero to retain the dialog after validation fails. Input,
clipboard actions and application shortcuts cannot reach the underlying page.
Dismissal restores its former control when it is still valid. Transparent
structural backdrops consume pointer input without adding dimming or effects.
Overlays supply their own Graphite font/ink inheritance on LVGL's separate top
layer; dialog inputs reflow when display density changes.
Wheel and fractional trackpad input hit-test layers in the same order as the
pointer, so dropdowns scroll under the cursor and overlay backdrops block
scrolling of the page behind them. Opening or closing a modal group clears
any fractional motion left over from the previous context.
`python3 ports/lvgl/menu-scroll-test.py --build build/lvgl --guest-build fuse-x/build`
checks the language picker and the 65-option dropdown at both densities in
native windows and desktop mode, using fresh offline test images.
`ui_radio_list` provides up to sixteen exclusive choices. Mouse clicks and
Enter select one choice; arrows move between enabled choices, and Tab/Shift-Tab
traverse controls. Selecting an already selected choice keeps it selected.
`ui_radio_selected` reads the selection, and `ui_radio_select` changes it.
One overlay may be open per application. The shared input convention copies
or cuts the whole field; Select All marks it for replacement on typing/paste.

### Lucide icons

The shared layer provides 21 named Lucide icons. `ui_icon(parent, "folder")`
creates a 16-point icon; `ui_icon_set` changes its artwork, and
`ui_icon_button` / `ui_row_icon_labels` compose icons with ordinary controls.
`ui_icon_count` and `ui_icon_name` enumerate the set. Unknown names return NULL
from constructors, or zero from `ui_icon_set` without changing the current icon.
Icons inherit text colour, including primary and disabled states. They do not
take focus, intercept clicks, or add Tab stops. Density changes select authored
16/32-pixel masks without rebuilding widgets. The selector uses icons for
navigation, actions and folder/file rows; its labels and keyboard controls remain.

Artwork comes from the pinned SVGs in `ports/fusede/icons/lucide` (Lucide 1.59.0,
revision `1ef60af944de207e0d5176905a0759ba12e6ee8d`). `arrow-up` is a clockwise
quarter-turn of `arrow-left`. Both the ISC licence and Feather-derived MIT
notice are retained in `icons/LICENSE.Lucide.txt` and staged with the apps.
These are new antialiased A8 masks, not the older FuseDE binary atlas. They use
26,880 logical octets of coverage data. The validated static-glyph renderer is
an internal transport; applications use names, without private codepoints or
an icon-font dependency in their API. No guest SVG renderer or rasterizer runs.

Ordinary builds verify pinned source hashes, masks and generated tables. Asset
regeneration is maintainer-only and uses sharp 0.35.5, libvips 8.18.7 and librsvg
2.63.2 on the host, with 4× supersampling and no binary threshold:

```sh
LVGL_SHARP_MODULE=/path/to/sharp node ports/lvgl/icons/rasterize.mjs
python3 ports/lvgl/icons/build.py
```

`icons/source.json` records provenance, generation versions and output hashes.
Inspect both densities and run the gallery/selector tests after regenerating.

## Calculator

Type expressions such as `(2+3)*4`, or use the mouse buttons. Enter evaluates;
Escape or C clears; Backspace/Delete and Left/Right/Home/End edit the field.
Tab and Shift-Tab move between controls; Enter activates a focused button.
The numeric keypad also works. After a result, an operator continues from that
result and a digit starts a new expression.

Arithmetic supports `+ - * /`, parentheses, unary signs, decimal numbers
and exponent notation. Expressions are limited to 128 characters and 24 nested
parentheses. Errors leave the expression available for correction. The native
Edit menu copies or cuts the whole expression, pastes numeric expression text,
and selects the expression for replacement. Paste is validated before insertion.

The default window is 300×360 logical points and resizes from 260×320 to
1920×1080. Each process
owns its own window and calculator state. `--trace` writes diagnostic input,
result and layout messages to the launching terminal.

## File selector

`lvselect [DIRECTORY]` starts in the supplied folder or the current directory.
Folders sort before files. Click to select, double-click or use Open to enter
a folder or choose a regular file. Up, Home and Refresh provide folder controls;
Home respects `$HOME`. The checkbox shows hidden files. The path field accepts
either a folder or a file; Go or Enter opens it. Failures preserve the listing.

With the list focused, Up/Down, Home/End and PageUp/PageDown move selection;
Enter opens, Right enters a selected folder, and Left/Backspace goes to its
parent. Tab/Shift-Tab move between controls. Ctrl+L or Command+L selects the
path for replacement, F5 refreshes, and Escape cancels. Mouse wheel and
fractional trackpad input scroll the list under the pointer. The native Edit
menu copies/cuts the whole path, pastes one path, and selects it for replacement.

Like `nxselect`, it prints the selected absolute path as one line on stdout,
or `[]` on cancellation/window close, with status zero. Startup or graphics
failures return nonzero. `--trace` sends diagnostics to stderr so stdout stays
usable by a caller. This separate executable does not replace system nxselect.
It selects existing regular files; it is not a save dialog or file manager.

Paths are limited to 511 logical UTF-8 octets, and a listing to 1024 entries
including its parent row; a status message identifies a truncated listing.
The current guest `dirent` API bounds each filename to 30 C16 cells. UTF-8 path
input is preserved. The bundled font covers Latin-1, Greek, Cyrillic (including
Ukrainian) and selected punctuation; characters outside the compiled subset
can still appear as boxes. The default window is 640×480 logical points and
resizes from 480×340 to 1920×1080.

The shared adapter acknowledges native window geometry up to the hosted GX
limits (1920×1080 logical points, or 3840×2160 backing pixels at 2×). An older
1024-pixel client-width cap could leave a black region inside a wider fullscreen
window; it was not an aspect-ratio policy. The strip-buffer budget remains
262,144 pixels at 1× and 1,048,576 at 2×, independent of window width. Wider
windows use shorter rendering strips instead of allocating larger draw buffers.
The shared table's bounded cache supports forty rows by thirty-two columns,
covering minimum-width columns across the wider viewport. Its text-mask memory
limit is unchanged. `wide-screen-test.py` compares the complete client image at
1470×900 and 1920×1080, both densities, and checks far-right clicks, last-column
navigation, wheel reuse, device bounds and returning to a smaller window.

## Data viewer

`lvdata [--plot|--no-plot] [FILE]` opens CSV, TSV or semicolon-separated UTF-8
text without changing its source file.
Open (Ctrl+O) uses the same Graphite file dialog as Text Editor and plot export.
Browse directories or enter a complete path; no sibling executable is required.

Arrow keys select cells; Home/End select the first/last column and Ctrl+Home/
Ctrl+End the first/last cell. PageUp/PageDown move one viewport. Both axes also
scroll with the pointer. Click a column heading to sort ascending/descending;
drag its right edge to resize it. Numeric columns sort numerically if every
nonempty value parses as a finite number; other columns use UTF-8 byte order.
Sorting is stable, and empty or space/tab-only values stay last in both directions.
View → Original row order restores file order. Sorting never rewrites the file.

Ctrl+C (or native Copy) copies a cell; Ctrl+Shift+C copies the entire row as
quoted TSV. Toolbar buttons provide the same actions. Embedded newlines,
separators and quotes are retained when copying, while cell previews replace
control characters with spaces. Copy reports selections exceeding the 32 KiB
clipboard limit instead of silently truncating them.

F5 reloads, F8 opens Options, F10 focuses the menu bar, and Ctrl+W closes.
Options uses a radio-button list for automatic/comma/tab/semicolon parsing,
and chooses whether the first row
contains column names. The parser supports doubled quotes, embedded quoted
newlines, CRLF/LF/CR, a UTF-8 BOM, ragged rows and empty fields. Delimiter
autodetection counts separators in the first quote-aware record; choose one
explicitly when that record is ambiguous. Invalid quoting, invalid UTF-8/NUL,
unreadable files and exceeded bounds leave the previous document intact.

The current bounds are 4 MiB of input octets, 100,000 data rows, 64 columns and
one million fields. C16 stores each input octet in a 16-bit addressable cell,
so guest memory use is larger than the file size. Loading, parsing and sorting
run cooperatively in bounded batches, returning to input/rendering between
steps. Progress and cancellation describe these real jobs. The old document or
row order remains intact until a successful commit; cancelled/error jobs do not
leave a half-parsed document or partially sorted table.

Ctrl+P or View → Plot opens a vertically resizable table/plot workspace. Select
an X column (or Row number), a Y column and Line/Scatter mode. Missing,
non-numeric and nonfinite values are reported and remain gaps rather than
invented zeroes. Plot preparation yields between bounded batches. Clicking a
plotted sample selects its original source row in the table; sorting rebuilds
the mapping without treating decimation buckets as rows. Fit, drag, arrows and
zoom share the reusable plot component. Ctrl+E or Export opens a validated Save
dialog for a standalone SVG, with replacement confirmation for existing files.
This exports the plot and does not rewrite the CSV.

`$HOME/.lvdata` stores the header/delimiter options, 64 column widths, pane
visibility/dividers, plot columns/mode and eight recent absolute paths.
It uses a temporary file and rename. It stores no cell
contents. Without HOME it runs without saving preferences; save failures appear
in the status bar. The staged synthetic example is
`/mnt/host/lvgl/examples/measurements.csv`.

## Text editor and document components

`lvedit [FILE ...]` opens up to sixteen documents. Its `ui-document` model owns
the existing TextBuffer gap buffers, file identities and per-tab selections.
`ui-editor` is a graphical viewport over a borrowed buffer, not a second LVGL
textarea model. `ui-tabs` is the shared document-tab strip: compact integrated
tabs, copied labels, a visible dirty indicator, keyboard selection and
application-owned close requests. Each closable tab reserves a small slot on
the left; its close button appears when the tab is hovered or keyboard-focused,
without moving the title. Long labels are ellipsized, and the strip scrolls
horizontally to keep the selected tab visible. The gallery's Editor page (F9)
uses the same component, including wrap and independent undo.

The editor keeps File/Edit/View/Help above one document area containing the
tab strip directly against the text viewport. File paths and duplicate quick
actions do not occupy separate rows; the existing menus and shortcuts provide
New/Open/Save/Undo/Redo. Closing a tab still goes through the document model and
its unsaved-work decisions, whether requested with its close button, Delete
while the tab is focused, or Ctrl+W.

Ctrl+N creates a document, Ctrl+O opens a path, Ctrl+S saves, Ctrl+Shift+S saves
under another name, Ctrl+W closes a tab and Ctrl+Q quits. Ctrl+Tab and
Ctrl+Shift+Tab change tabs. Ctrl+A/C/X/V select/copy/cut/paste; Ctrl+Z and
Ctrl+Shift+Z undo/redo. F5 reloads from disk and F10 focuses the menus. The View
menu toggles visual wrapping. File contents do not acquire line endings when
the viewport wraps.

Modified documents with an existing path autosave when the text area loses
focus, when switching away from their tab, or when the editor window loses
focus. Untitled documents need a manual Save As first. Saves run outside focus
callbacks and preserve the cursor, selection and undo history. Explicit Save
As, Reload and unsaved-close decisions take precedence over queued autosaves.
External changes and write failures leave the document dirty and show a status
warning; an explicit Save can resolve the conflict or retry the write.

View → **Syntax language…** selects highlighting for the current document,
including unsaved tabs. **Automatic** uses its filename; an explicit choice
overrides detection until Automatic is selected again. Choices are per tab and
remain in effect through Save As and reload while the document is open. They
do not change file contents, undo history or the dirty indicator, and are not
stored in the file or restored after restarting the app. The status bar shows
the current language. Highlighting uses Rift's existing text-engine lexers.

The viewport measures actual glyph advances, retains grapheme-safe selection
and hit testing, and draws visible syntax runs. Display-row layout proceeds in
bounded batches and pauses while idle; caret-only movement damages its old/new
strips when selection and scrolling are unchanged. Editing a logical line
rebuilds its wrapped suffix. Explicit file-end navigation can request layout
synchronously. Existing font coverage and shaping limits still apply.

Documents retain the text engine's 512 KiB per-file and aggregate allocation
bounds. The clipboard remains the GX 32 KiB text profile; rejected oversized
copies/cuts leave the document intact. Pasted batches form one undo group.
Dirty close and native window-close requests offer Save/Discard/Cancel.
Serial quit confirmation preserves remaining documents when cancelled.

Loads and reloads commit only after a complete successful read. Saves create
a short exclusive temporary filename in the destination directory and rename
it after successful writing, preserving file mode and guest filename limits.
Changed/replaced/deleted disk files require an explicit overwrite or save-copy
decision. Saves refuse symbolic-link destinations. Fingerprints and a final
recheck narrow races; they are not filesystem locks or power-loss guarantees.
Wrap and eight recent paths are saved atomically in `$HOME/.lvedit` (0600).

For desktop testing, `scripts/run-fuse-x.sh --desktop` rebuilds and installs
the apps and Graphite shell. `--run-only --desktop` uses the apps already in
the image. The same executables can be launched from the desktop's terminal;
the future x64 output adapter remains a separate milestone.

## Retina text and Unicode

All five apps follow 1×/2× display density through `GX_BACKING_SCALE`. A 640×480
selector retains that logical window size while rendering 1280×960 pixels at
2×. Mouse events and resize requests stay logical; the adapter maps them into
LVGL's physical coordinates. Moving between densities reapplies geometry and
fonts without rebuilding the document or losing entered text. Fixed-size
windows can change backing density too, and pending logical resizes survive.
The macOS frontend supplies its actual backing factor; headless macOS tests
start with a 2× preference and can simulate density changes.

Noto Sans is bundled at 12/14/16 and 24/28/32 pixels as antialiased A8 masks,
covering 727 codepoints. Identical glyph images within each size share bitmap
storage without changing advances, offsets or pixels. This keeps all font roles
inside the current guest static-data limit. LVGL Montserrat/Font Awesome at
14/28 pixels remains an icon
fallback. The checkbox mark is expanded from the pinned Font Awesome A4 source
to static A8 at build time, preserving its original pixels and avoiding the
packed-glyph path on C16. `fonts/symbols.json` pins those source members. Other
packed fallback symbols are outside the current guest acceptance profile.
Upstream symbol strings use escaped UTF-8 octets. Source preparation decodes
these into literal Unicode characters because C16 encodes string characters,
including hex escapes, as UTF-8; otherwise each octet is encoded twice. The
guest font check verifies the checkmark's three octets and decoded codepoint.
ASCII and basic Cyrillic use direct character maps; other covered
characters use a sparse map. Masks are generated once from a pinned OFL font,
then compiled into the guest. Neither FreeType nor HarfBuzz runs in the guest.
This is simple glyph layout, without OpenType shaping, automatic font discovery
or complete Unicode/emoji coverage.

`fonts/source.json` pins the source revision and checksums; `fonts/generated.json`
records coverage and generator versions. Normal builds use checked-in font
tables. Regeneration requires Pillow and the cached checksum-verified LVGL archive,
and runs `python3 ports/lvgl/fonts/generate.py`.
The Noto Sans license is staged alongside the apps.

C16 builds pack two A8 coverage octets into each static uint16 cell, then
expand the six faces once into immutable heap buffers before LVGL starts.
Glyph metrics, coverage, fallbacks and `static_bitmap` lifetimes remain exact;
shutdown releases the buffers after LVGL teardown. This saves 431,554 static
cells (863,108 physical bytes) without changing the executable memory layout.
Allocation failure rolls back all faces. Host references use the original
unpadded A8 tables; the lossless suite checks all 863,112 coverage octets,
glyph descriptors, repeated lifetimes and every allocation failure point.

For comparison or an older display, launch with `LVGL_SCALE=1` or
`LVGL_SCALE=2`. Without an override, density follows the display. Older kernels
without the capability use 1× rendering. Restart Fuse-X after rebuilding its
kernel/runtime to activate the new capabilities.

## Adapter and rendering profile

`gx-port.h` separates the UI from window creation, input, clipboard, timing,
partial uploads and teardown. `gx.c` uses `/dev/gfx`; `host-gx.c` is a test-only
reference adapter. Widget layout, font lookup, arithmetic and draw-task
generation execute inside the guest. GX supplies optional bulk pixel operations.

The adapter discovers surface formats through `GX_FORMATS` and chooses opaque
`GX_XRGB8888` when direct BGR upload and full-colour surfaces are available.
All eight bits of each colour channel survive upload, copy, resize, publication
and native presentation. This avoids RGB565 quantization and the subsequent
5/6-bit expansion at display time. Native screenshots use the same full-colour
published pixels. `LVGL_RGB565=1` forces the legacy format for comparison;
`LVGL_SOFTWARE_UPLOAD=1` also selects RGB565. Unsupported/older backends keep
the existing RGB565 path. RVM/RBF graphics are unchanged.

The profile enables RGB888 drawing, A8 masks, bundled compact text roles,
buttons, labels, text areas, checkboxes, and flex/grid layouts. LVGL 9.6
deprecates `lv_list`: the selector uses its recommended stock flex container
with button children. Packed RGB565 drawing is
unsuitable for the current C16 memory representation. Each RGB888 channel
instead occupies one logical C character. The adapter uses the optional
`GX_UPLOAD_BGR888` operation to copy borrowed B,G,R source cells directly
into the host's chosen surface format. Older kernels use guest conversion and
ordinary RGB565 upload. `GX_PRESENT` remains the atomic presentation boundary.
The draw buffer is bounded independently of window height at 786432 C16 cells
at 1× (1.5 MiB physical), and 3145728 cells at 2× (6 MiB). Software upload adds
262144/1048576 cells for conversion (512 KiB/2 MiB); direct upload does not
allocate that buffer. Widget allocations and host front/back surfaces require
additional memory. Larger strips reduce
repeated clipping, object traversal and glyph processing during scrolling.
Hosted full-colour front/back surfaces use eight bytes per physical pixel in
total, compared with four bytes for RGB565; guest draw-buffer sizes are unchanged.
The guest suites compare the complete RGB888 output byte-for-byte with
the host reference, rather than accepting equality after colour quantization.

The optional `GX_RENDER` interface keeps bounded colour and A8 targets in the
renderer, identified by opaque handles owned by one window. Its command batches
provide fill, borrowed A8 masks, retained A8 masks and overlapping copies; upload,
readback, curve coverage and window blits are explicit operations. All commands
and borrowed spans are validated before a batch changes pixels. Window teardown
releases its targets, and old handles cannot address replacement targets.

LVGL's adapter uses one reusable native colour strip for supported opaque and
masked draws. It batches the small masks and leaves pixels in ordinary native
storage until copying the strip to the window. This avoids the repeated guest
RGB writes and subsequent full strip RGB uploads. The plot also retains its
native curve mask between draws. It preserves the portable C16 mask when a new
curve is prepared, so a later unsupported draw can still use the software path
without different plot pixels. Layout, clipping, widget state, typography and
application data remain in the guest.

An unsupported draw, such as an ordinary RGB888 image, first submits pending
commands and reads the current colour strip back into LVGL's buffer. LVGL then
completes that strip in software and uploads it normally; the next strip can use
native commands again. Existing kernels automatically retain the original
adapter. `LVGL_SOFTWARE_TARGET=1` explicitly selects that path, and the existing
software-upload, software-raster and RGB565 diagnostics also disable retained
targets. Exact 1x/2x scene comparisons include masked text before and after an
RGB image spanning several strips. Pure native scenes and plot benchmarks also
check that they issue no colour readbacks or RGB uploads.

The allocation-free CPU primitives live in `libraries/graphics/render.rs`, with
conventional eight-bit coverage and 32-bit colour words. They know nothing about
C16 cells, encoded guest pointers or macOS windows. Fuse-X supplies allocation,
handle ownership and presentation; a future Riplix backend can reuse the core
behind its own application ABI. This work does not modify the ongoing x64 kernel
or add an LVGL/SDL dependency. See the
[retained rendering contract](../../docs/graphics/retained-rendering.md).

The optional `GX_RASTER_BGR888` operation fills/blends BGR caller RAM using
an optional A8 mask. The guest still supplies glyph masks, shapes and colours.
Kernel validation covers complete source/destination spans; host row transfers
preserve page/COW isolation and retain no pointers. Unsupported kernels use
the C16 software blender. `LVGL_SOFTWARE_RASTER=1` forces that path for testing.

The hosted blender skips empty A8 rows and transfers destination BGR data only
around nonzero coverage spans for sparse masks. Nearby short gaps are grouped
to avoid excessive page lookups; dense masks retain bulk row transfers, and
fully opaque spans copy their prepared colour without reading the destination.
Mask low-byte interpretation, opacity thresholds, blend rounding, row padding
and copy-on-write snapshots retain the same semantics. All borrowed spans are
validated before any write, including requests whose opacity is zero.

Plot coverage images use this existing bulk blend operation, with one masked
blend per intersecting display strip. The former skew-line path produced a
separate blend and graphics transition for nearly every covered scanline of
each segment. Optional `GX_RASTER_A8` separately accelerates generation of the
coverage image from a bounded list of lines/circles. The adapter probes support
before use. The operation snapshots and validates the whole primitive list and
all borrowed spans before changing RAM; clear/overlay requests preserve row
padding and copy-on-write isolation. Its native scratch image uses one byte per
physical pixel and is released before returning. It does not select colours,
publish windows or interpret application data. Coverage generation and BGR
blending each retain an independent software fallback; restart Fuse-X with the
updated kernel to activate the new backend operation.

There is one upload per damaged rectangle and one presentation per completed
refresh. The event loop waits for input or the next LVGL timer deadline.
Input is drained in bounded batches, with press/release events processed
individually. Focus loss and queue overflow release held input. Wheel events
retain fractional motion and route to the scrollable object under the pointer.
Folder changes are deferred outside input callbacks before rebuilding rows.

`adapt.py` creates a separate patched source tree under `build/lvgl/src` from
the checksum-verified release. It records adaptations in `adaptation.json`:

* Scalar RGB888 stores and channel arrays replace packed octet word operations.
  Opaque fills seed one BGR pixel, grow it with non-overlapping bulk copies,
  and copy the completed row instead of performing checked scalar stores for
  every channel of every pixel. Row padding stays unchanged.
* Coordinate/flag shifts use wide literals; color shifts use explicit widths.
* Reverse style loops explicitly widen before subtracting, preserving their
  zero-count behavior on C16.
* Wide flags, widget parts and event identifiers have explicit storage types.
* Numeric union initializers specify their member width. A layout callback
  structure is named and an aggregate array is explicitly zeroed to avoid
  current frontend limitations.
* In-memory image classification reads the low magic octet when C16 packs
  another header field into the same addressable cell.

The compiler is unchanged. Fuse-X's kernel and hosted graphics runtime add the
optional density, coverage, blend and upload operations; the established RGB565 ABI
remains available and unscaled for clients which do not opt into density.
This validates a bounded profile, not every LVGL widget or renderer.
ARGB layers, arbitrary transforms, external image decoders, runtime font
engines, XML and filesystem drivers are outside this profile. The calculator
accepts numeric ASCII; it is not a rich text editor.

Fuse-X is the integration target. The adapter boundary can be reused when
Riplix x64 obtains its native application and graphics contracts. RVM is not
a performance or acceptance target for this port.

## Verification

Requirements: Python 3.12+, the repository's RCC/Fuse-X test kernel and shell
artifacts, Node.js, and a host C compiler with a working SDK. Run:

```sh
python3 ports/lvgl/build.py
python3 ports/lvgl/host-test.py
python3 ports/lvgl/test.py
python3 ports/lvgl/selector-test.py
python3 ports/lvgl/selector-test.py --software-upload
python3 ports/lvgl/test.py --retina
python3 ports/lvgl/selector-test.py --retina
python3 ports/lvgl/selector-test.py --retina --software-upload
python3 ports/lvgl/selector-test.py --retina --rgb565
python3 ports/lvgl/retina-test.py
python3 ports/lvgl/gallery-test.py
python3 ports/lvgl/gallery-data-test.py
python3 ports/lvgl/workspace-test.py
python3 ports/lvgl/document-model-test.py
python3 ports/lvgl/ui-editor-test.py
python3 ports/lvgl/font-data-test.py
python3 ports/lvgl/browser-scan-test.py
python3 ports/lvgl/editor-test.py
python3 fuse-x/test_desktop_mode.py
python3 ports/lvgl/forms-test.py
python3 ports/lvgl/gallery-plot-test.py
python3 ports/lvgl/workbench-test.py
python3 ports/lvgl/csv-model-test.py
python3 ports/lvgl/csv-job-test.py --guest
python3 ports/lvgl/mask-raster-test.py
python3 ports/lvgl/render-target-test.py
python3 ports/lvgl/data-test.py
python3 ports/lvgl/table-scroll-test.py
python3 ports/lvgl/table-fullscreen-test.py
python3 ports/lvgl/wide-screen-test.py
```

After building, `python3 ports/lvgl/verify.py` runs the host and guest suites,
serializes cases that share helper outputs, and records the tested executable
hashes in `build/lvgl/verification/result.json`. It does not rebuild or stage
the product executables. Set `FUSE_X_TEST_KERNEL=/path/to/fuse-x` to test an
isolated candidate kernel. The native mask and retained-target suites require
the new operations; old-kernel fallback is checked separately by the drag
benchmark. The retained suite checks banked colour/A8 round trips, untouched
padding, every unmasked fill opacity and all 65,536 mask/opacity pairs,
sequential overlapping copies,
transactional invalid requests, ownership across fork, COW readback, stale
handles, RGB565 window destinations, resize and teardown. Its second stage
compares retained and forced-software LVGL scenes exactly at both densities,
including ordinary RGB image fallback across several strips. Test-only scene
and benchmark binaries are built privately and are never staged as apps.

The host test builds the same adapted LVGL profile and all five UIs under
UndefinedBehaviorSanitizer. It includes forms, plot-model, plot-raster and plot-widget
component checks; renders the Plots/Forms gallery pages and line/scatter
workbench references at 1x/2x; and tracks their source hashes in
`build/lvgl/host/*-result.json`. Include-based widget tests invalidate their
cached object when the included implementation changes. The guest test creates a fresh, offline disk from
an explicit manifest, compares the initial rendered full-colour pixels, exercises
arithmetic, mouse/keyboard/clipboard input, resizing, idle presentation and
independent window lifetimes, then shuts down and removes that disk.
Reports and screenshots are under `build/lvgl/tests`; the host reference is
`build/lvgl/host/reference.ppm`. The selector's independent test checks the file
model, initial full-colour rendering, traversal, scrolling, path input, keyboard
focus, checkbox, refresh, clipboard/UTF-8 editing, resizing, idle frames, output
protocol and clean cancellation. Its outputs are in `build/lvgl/selector-tests`
and its host reference is `build/lvgl/host/selector.ppm`. Both tests shut down
and check their disposable filesystems. A host screenshot alone is not evidence
of a working guest port. The gallery suite compares exact 1×/2× guest pixels,
then checks live/disabled actions, hover, Unicode editing/clipboard, keyboard
checkboxes, page/draft preservation, list keys/wheel, resizing, density changes,
idle publication and close cleanup. Outputs are in `build/lvgl/gallery-tests`. The Data gallery and viewer suites
add modal focus/validation, menu traversal, real child file picking, exact
clipboard payloads, parsing failures, persistent options/column widths/recents,
100,000-row navigation and wide-file horizontal scrolling. Reports live in
`build/lvgl/gallery-data-tests` and `build/lvgl/data-tests`. The
Plots/Forms gallery suite additionally checks source-row peak selection,
choice confirmation/Escape, Open validation, Save replacement intent without
file writes, cancellation/restart of actual bounded work, page replacement and
idle publication. Its report is in `build/lvgl/gallery-plot-tests`. The horizontal
component suite uses actual pointer column resizing, fractional wheel input,
partial-column header/gutter/cell pixel comparisons at 1×/2×, recycled-header
sorting/resizing and saved widths. Its report is in
`build/lvgl/table-scroll-tests`. The host suite additionally checks stable
header identities, pixel scrolling without layout invalidation, all 64 columns,
minimum/maximum widths, hidden-header updates, replacing/emptying a document and
logical viewport preservation across density changes under UBSan
(`build/lvgl/host/table-component-result.json`). It also exercises the mask
budget, offscreen eviction, cache cleanup and overlapping-alpha fallback. The
fullscreen guest suite compares a 1024×1024 logical viewport at 1×/2×, recycles
both axes, checks last-row/column and restored-home pixels, and returns to a
smaller window (`build/lvgl/table-fullscreen-tests`). The CSV model is
also tested in C16, with 150 generated host cases compared against Python csv
under UndefinedBehaviorSanitizer.

The plot regression checks also exercise exact right-edge normalization,
clipped segments, all raster octants, extreme coordinates, antialias coverage,
1×/2× strokes, circles and mask padding. The right-edge normalization now uses
explicit floating-point endpoint literals locally; an integer literal at that
boundary could be returned incorrectly by the current C16 native path and
pull the last segment across the graph. This correctness fix is separate from
the optional graphics acceleration and does not change the compiler.
The native mask guest suite compares CPU/native coverage byte for byte across
bank boundaries, strided padding and 8192 primitives. Invalid versions, spans,
overlap and a malformed final primitive must leave the destination unchanged.
Axis-label cache tests cover reuse, font/text changes, resizing, fallback and
the memory budget; six ordinary/cached label references remain byte-identical.

The full-colour suites compare every RGB channel exactly with 1×/2× host
references. Forced RGB565 suites compare quantized pixel values. Kernel checks
also capture precise low-bit colours after padded cross-bank upload, RGB fill
and overlapping copy, while checking invalid requests and process ownership. A
separate fresh-disk test simulates display changes while editing Ukrainian
text, checks path/expression preservation, logical clicks, resizing and fixed
scale overrides. Runtime tests cover all 65536 mask/opacity combinations,
padding, invalid/overlapping spans, physical surface bounds, immutable published
frames and density changes alongside pending resizes. Test disks with both
static apps need more room for their embedded fonts and use 128 MiB images.

## Plot dragging measurements

`plot-drag-test.py` measures continuous pointer drags through the production
shared plot widget, LVGL input handling, C16 geometry and real Fuse-X
coverage/blend/presentation path. It uses a fresh explicit-manifest offline disk
and disables screenshot capture, so converting and writing PPM files does not
distort rendering measurements. It does not open personal disk images or stage
the benchmark executable.

```sh
python3 ports/lvgl/plot-drag-test.py --build --frames 32 --runs 3 --output build/lvgl/plot-perf/current.json
# Compare against an immutable benchmark binary retained before a change:
python3 ports/lvgl/plot-drag-test.py --baseline /path/to/before.fxe --rows 40 --frames 32 --runs 3 --output build/lvgl/plot-perf/comparison.json
# Exercise the largest supported logical window explicitly:
python3 ports/lvgl/plot-drag-test.py --size 1920x1080 --scale both --rows both --output build/lvgl/plot-perf/large-window.json
```

The default matrix covers 800×560 and 1470×900 windows at both densities,
with a 40-point sample and a deliberately dense 100,000-point oscillating
series. The plot fills almost the whole benchmark window; a Data Viewer
table/plot split usually has a smaller plotting area. `--events-per-frame 16`
exercises multiple real pointer events before each refresh. Reports separate
input, geometry preparation, refresh, upload and presentation costs, and record
frame-time percentiles and executable hashes.
Use `--kernel /path/to/fuse-x` to select an isolated candidate kernel; reports
record its hash too, so application and backend changes can be measured
separately. `--require-retained` requires positive native command and resident
A8-mask counts, with zero colour readbacks, fallback strips and RGB uploads.
`--software-target` selects the portable draw-buffer path for the same binary.
These two diagnostics are mutually exclusive.

Refreshes are forced without scheduler waits to measure a sustainable
throughput ceiling. The applications retain their 33 ms refresh interval;
benchmark throughput is not an on-screen frame-rate claim and excludes native
compositor work. Interactive acceptance also checks real pointer input,
clipping, source-row selection, gaps, both densities and idle publication.

Before retained targets, the 21 October 2026 coverage/text-cache comparison
used 32 frames and three alternating runs per configuration, with both the
original scanline renderer and its replacement using the same updated kernel. Medians of the three processing-throughput and
per-run p95 measurements for the 40-sample drag were:

| Logical window | Density | Before → after throughput | Before → after p95 |
| --- | --- | ---: | ---: |
| 800×560 | 1× | 26.7 → 146.1 frames/s | 42 → 7 ms |
| 800×560 | 2× | 15.2 → 80.8 frames/s | 72 → 13 ms |
| 1470×900 | 1× | 16.8 → 79.4 frames/s | 62 → 14 ms |
| 1470×900 | 2× | 8.9 → 40.7 frames/s | 126 → 26 ms |

All four ordinary cases leave processing headroom for the application's 33 ms
refresh interval. With eight pointer samples before each refresh, the large
2× case measured 38.6 frames/s and p95 27 ms; pointer processing itself never
exceeded 2 ms per frame. The chart's appearance is preserved: six cached-label
and ordinary-label references compare byte for byte at 1×/2×.

The deliberately dense 100,000-sample series measured 53.2 frames/s with p95
21 ms at 800×560/2×, and 23.1 frames/s with p95 45 ms at 1470×900/2×. The latter
full-window adversarial case remained below the 30 Hz target in that profile. A smaller
table/plot split has substantially fewer pixels; these results do not establish
a 30 Hz guarantee for every dataset and window size.

The identical final executable also ran against the previous kernel, checking
capability probing and the portable rasterizer. At 1470×900/2× it measured
31.1 frames/s for 40 samples, but only 5.8 for the dense series. Restart Fuse-X
after updating its executable to use the optional native coverage operation and
sparse masked blending. The staged applications also work with older kernels.
The source, compiler, executable and kernel hashes are recorded alongside
`labels-final.json`, `labels-paired.json`, `labels-burst.json` and
`labels-fallback.json` in `build/lvgl/plot-perf`; staged copies are named
`fuse-x-shared/lvgl/plot-drag-{final,paired,burst,fallback}.json`.

### Retained target comparison

The subsequent retained-target comparison freezes the previous cached-curve
executable and runs it alongside the new adapter against the same candidate
kernel. It uses the same 32-frame workload, three alternating runs and uncapped
refresh policy. The Retina cases measured:

| Logical window | Samples | Previous → retained throughput | Previous → retained p95 |
| --- | ---: | ---: | ---: |
| 800×560 | 40 | 72.4 → 95.5 frames/s | 16 → 12 ms |
| 800×560 | 100,000 | 36.0 → 43.6 frames/s | 35 → 27 ms |
| 1470×900 | 40 | 34.1 → 54.2 frames/s | 33 → 22 ms |
| 1470×900 | 100,000 | 18.5 → 30.4 frames/s | 60 → 36 ms |

At 1×, the corresponding native cases measured 151.7/89.6 frames/s at
800×560 and 89.1/46.5 at 1470×900, for 40/100,000 samples respectively.
The dense 1470×900/2× workload with eight input events before each refresh
measured 30.2 frames/s, p95 37 ms. Every native case issued retained A8-mask
commands and zero colour readbacks, fallback strips or RGB uploads. A curve
change still prepares a C16 A8 shadow for portable fallback; these measurements
do not imply that all guest mask traffic has been removed. The retained
`upload_us` profile field includes pending batch execution and window blits.

The maximum 1920×1080/2× backing has 8.3 million pixels and remains a stress
limit: 40 samples measured 23.7 frames/s (20.4–32.8 across runs), p95 58 ms;
the dense series measured 15.6 frames/s, p95 69 ms. Its dense per-frame work
was approximately 10.5 ms geometry, 22.7 ms coverage/shadow preparation and
30.5 ms refresh. This first retained profile leaves further CPU work to reduce
and does not establish a 30 Hz guarantee at every backing size. At
1470×900/2× the dense native case spends approximately 5.3/8.9/18.5 ms in
those same phases, compared with 5.7/8.6/39.8 ms in the previous adapter.

The same new executable also passes forced-software and previous-kernel runs,
using the existing RGB upload path without retained target commands. Raw
reports and immutable baseline hashes are under `build/lvgl/render-perf`.
Staged reports use `fuse-x-shared/lvgl/render-drag-{paired,burst,large-window,software,old-kernel}.json`,
with the comparison in `render-drag-summary.json` and `render-drag-summary.txt`.

## Virtual table scrolling measurements

The first CSV viewer bounded widget count, but recycled all visible labels on
scroll. Their text/position changes repeatedly invalidated layout across the
screen. The current table paints its cached viewport directly and retains only
its header widgets. Text widths and row-number positions are cached too.

Three fresh-process runs per case on the development machine (21 October 2026):
medians for 30 forced scroll redraws, without frame capture or the native compositor.
These measure headless rendering throughput, not on-screen frame rate.

| Dataset | Density | Before | After | Speedup |
| --- | ---: | ---: | ---: | ---: |
| 40 rows, 5 columns | 1× | 8.265 s | 0.714 s | 11.6× |
| 40 rows, 5 columns | 2× | 11.158 s | 0.780 s | 14.3× |
| 100,000 rows, 3 columns | 1× | 4.578 s | 0.642 s | 7.1× |
| 100,000 rows, 3 columns | 2× | 5.097 s | 1.092 s | 4.7× |

The sample's table widget count fell from 99 to 15. At 2×, layout work for the
30-frame sample fell from 4.805 seconds to roughly a millisecond. Upload/present
was already a small fraction of the total. The two datasets have different
column counts; their timings are not a row-count scaling comparison.

Reports: `build/lvgl/table-before-final.json`,
`build/lvgl/table-after-cached-widths.json`, and `build/lvgl/table-scroll-report.json`.
The report records ranges and executable hashes. Reproduce the current workload:

```sh
python3 ports/lvgl/build.py --app lvtablebench
python3 ports/lvgl/table-bench.py --runs 3 --output build/lvgl/table-scroll-current.json
```

The benchmark includes the actual viewer and parser, checks its real widget
count, and runs only in a fresh explicit-manifest offline disk. `--binary`
selects a previously built benchmark for comparisons; `--scale` and `--rows`
can restrict a run. `--axis horizontal` widens columns to 320 logical points;
`--column-width` changes that preset, and `--rows wide` exercises 32 columns.
`--baseline PATH` alternates fresh before/after processes in one test guest,
reversing their order on every other run. It records each executable hash;
use a baseline supporting the same workload parameters. Benchmark executables
are never staged as applications.

The horizontal follow-up fixes a different path: a trackpad may deliver many
small movements before the next redraw. Setting header positions for each one
forced layout during later input events and expanded invalidated regions.
Headers now scroll as a viewport, retain their identities across overlapping
columns, and rebind only incoming columns. Text fully hidden behind the fixed
gutter is skipped using cached glyph bounds, including overhangs.

With five columns widened to 320 points, three alternating before/after process
pairs measured the following medians for **30 redraws, with 16 small movements
per redraw** (same total distance as the single-event workload):

| Density | Before | After | Speedup | Input processing before → after |
| --- | ---: | ---: | ---: | ---: |
| 1× | 2.032 s | 0.619 s | 3.28× | 0.820 → 0.090 s |
| 2× | 2.538 s | 0.784 s | 3.24× | 0.886 → 0.098 s |

These remain headless throughput measurements. Single-event redraw gains were
modest; the main improvement is avoiding repeated work inside an input batch.
Vertical before/after medians remained comparable, with overlapping run ranges.
A separate 32-column stress case at the minimum 64-point width records the cost
of painting a denser viewport; glyph rendering still scales with visible text.
Reports: `build/lvgl/table-horizontal-report.json`,
`table-horizontal-burst-paired.json`, `table-horizontal-paired.json`,
`table-vertical-regression.json`, and `table-horizontal-narrow.json`.

```sh
python3 ports/lvgl/table-bench.py --axis horizontal --events-per-frame 16 --rows small --runs 3 --output build/lvgl/table-horizontal-current.json
# Add --baseline PATH to compare a retained benchmark built from older table code.
```

The fullscreen follow-up batches cached cell/row-number text into A8 images,
avoiding the per-letter draw pipeline on subsequent redraws. At a 1024×1024
logical viewport with a synthetic 200-row, 32-column file, paired medians for
30 redraws were:

| Axis | Density | Before | After | Speedup |
| --- | ---: | ---: | ---: | ---: |
| Vertical | 1× | 3.332 s | 1.478 s | 2.25× |
| Vertical | 2× | 2.639 s | 1.182 s | 2.23× |
| Horizontal | 1× | 2.096 s | 1.058 s | 1.98× |
| Horizontal | 2× | 2.488 s | 1.746 s | 1.43× |

Vertical uses 144-point columns and one movement per redraw; horizontal uses
320-point columns and sixteen small movements per redraw. These are separate
workloads. The masked-blend count, including the initial viewport render, fell
from 35,138 to 8,517 for the 2× vertical run and from 16,250 to 5,529 horizontally.
Three process pairs were run per case; the noisy 2× horizontal case was repeated
and includes all six pairs. Its after range is wide (1.544–5.666 s), so these
wall-clock medians do not guarantee a fixed fullscreen frame rate. Cache misses
still build new text runs and pixel work grows with the viewport. All 28 saved
pre-cache host reference images, including fullscreen, remained byte-identical;
real guest comparisons and cache-budget/overlap fallback checks also pass.
See `build/lvgl/table-fullscreen-report.json` and its three raw paired reports.

```sh
python3 ports/lvgl/table-bench.py --size 1024x1024 --rows wide --runs 3 --output build/lvgl/table-fullscreen-vertical.json
python3 ports/lvgl/table-bench.py --size 1024x1024 --rows wide --axis horizontal --events-per-frame 16 --runs 3 --output build/lvgl/table-fullscreen-horizontal.json
```

Guest integration additionally compares the initial, last-record and
last-column content at both densities, and verifies wheel scrolling down and
back through the cache against the host reference.

## Initial scrolling measurements

The first selector bottleneck was guest-side software rendering, rather than
window presentation. A deterministic 30-frame scroll over the same 67-entry
synthetic directory measured the following on this development machine
(20 October 2026, before the Noto/Retina work; medians of three fresh-process runs):

| Version | Time for 30 frames | Software rendering throughput |
| --- | ---: | ---: |
| Initial port | 3.779 s | 7.9 frames/s |
| Bulk fills, larger strips, direct BGR upload | 0.705 s | 42.6 frames/s |

This is about 5.4× faster for this workload. PPM capture is disabled during
timing; the test runs headlessly and does not measure macOS compositor/vsync or
promise an on-screen frame rate. LVGL's normal refresh interval remains 33 ms.
That change kept the font and rendered content unchanged. Pre-decoding the
original font did not produce a material gain and was not retained. The later
Noto subset uses static A8 glyphs to avoid runtime unpacking, and direct maps
for common text to avoid repeated binary searches in guest code.

With the Noto fonts and density support enabled, the 2× version of the workload
(68 entries, four times the pixel area, before the full-colour surface extension and compact Graphite layouts)
took a median 0.680 s for 30 frames
across three fresh-process runs on 21 October 2026, ranging from 0.652 to
0.879 s. That is 44.1 frames/s of headless rendering throughput; the same
compositor/vsync limits above apply. The generic BGR/A8 blend operation and
larger density-aware strips keep the additional pixels affordable.

The subsequent, pre-Graphite format comparison used the same 2× benchmark executable and
kernel, three fresh-process runs per format with capture disabled:

| Surface format | Median time for 30 frames | Range |
| --- | ---: | ---: |
| XRGB8888 | 0.688 s | 0.684–0.806 s |
| Forced RGB565 | 0.748 s | 0.682–0.775 s |

These overlapping ranges do not establish a speedup. They show comparable
headless throughput while retaining full colour. LVGL drawing remains the
dominant cost; upload/presentation is a small fraction of this workload.
Native compositor conversion/rendering is outside this benchmark. Full-colour
presentation also avoids the legacy RGB565 expansion used by the native view.
Reports are `build/lvgl/bench-truecolour.json` and
`build/lvgl/bench-truecolour-rgb565.json` (21 October 2026).

Reproduce the benchmark after rebuilding the Fuse-X kernel/runtime:

```sh
python3 ports/lvgl/build.py --app lvbench
python3 ports/lvgl/host-test.py --bench
python3 ports/lvgl/bench.py --output build/lvgl/bench-after.json
python3 ports/lvgl/bench.py --software-upload --output build/lvgl/bench-software.json
python3 ports/lvgl/bench.py --scale 2 --output build/lvgl/bench-retina.json
python3 ports/lvgl/bench.py --scale 2 --rgb565 --output build/lvgl/bench-retina-rgb565.json
```

`lvbench` is a test workload and cannot be staged by the builder. It also
checks small, odd and large fill widths, row padding and guard cells. The guest
selector tests check padded cross-bank uploads, bounds and process ownership
through the actual kernel, and compare rendered pixels with both upload paths.
Runtime tests additionally verify unpublished back surfaces and immutable
presented snapshots. Baseline and optimized reports are under `build/lvgl/`.

For an interactive diagnostic run, `LVGL_PROFILE=1 lvselect` prints cumulative
conversion/upload/presentation timings to stderr on exit.
`LVGL_SOFTWARE_UPLOAD=1` forces the compatibility path. Profiling is off by
default. Restart Fuse-X to use a rebuilt kernel's upload acceleration; the
staged apps still run with the software fallback in an older running session.

The earlier worksheet/table/chart investigation remains reproducible:

```sh
python3 ports/lvgl/probe.py all --fetch
python3 ports/lvgl/x11_audit.py --fetch
```

Its results live separately in `build/gui-evaluation/`. See the
[graphics assessment](../../docs/graphics/lvgl-and-x11.md) for that evidence,
font/editor limitations and the independent X11 investigation.

## Packaging direction

The standalone builder stages selected applications in `fuse-x-shared`, with
their shared license/readme folder, without opening disk images. The normal
Fuse-X `--desktop` launcher now builds/installs the five ordinary Graphite apps,
`fusedeshell` and its curated launch catalog. This system launch catalog is
separate from signed package distribution. Later, `lvcalc` can be
an optional package carrying `/usr/bin/lvcalc` and its licenses, built with this
same builder. The existing [package tools](../pkg/README.md) should create its
signed archive and catalog entry; the [RFL pkg frontend](../../rfl/apps/pkg/)
remains the user interface. Syma and selected GUI apps can follow the same path.
A versioned shared LVGL guest library can follow when several apps justify
that dependency; it is not required for this executable.

Upstream identities are pinned in [PROVENANCE.json](PROVENANCE.json). LVGL core
is MIT licensed. The bundled Montserrat and Font Awesome font notices are
extracted from the same verified archive and staged beside the executable.
