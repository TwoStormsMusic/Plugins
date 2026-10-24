# Porting the next TwoStorms plug-in

The Compressor is the template. Everything in `src/core` and `src/gui` is shared; everything in
`src/plugins/compressor` is what one plug-in adds. A new plug-in is a new folder next to it, one line
in `src/entry.cpp` and three lines in `CMakeLists.txt`. All plug-ins ship in the one `TwoStorms.clap`.

## The tree

```
src/core/            no GUI code; the audio thread lives here
  eel.h              EEL2 semantics (==, !=, truthiness, rounding) for ported code
  params.*           parameter table: IDs, ranges, enums, text <-> value (host and window)
  plugin.*           the CLAP plug-in every TwoStorms plug-in derives from
  act.*              the built-in Act, audio-thread side (the JSFX's a_* functions)
  act_view.*         the Act, window side (the JSFX's g_* functions)
  shm.*              shared memory between all instances on the machine
  presets.*          factory and user presets
  reaper_bridge.*    REAPER: placement, names, routing, drive link to a plug-in parameter, hooks
src/gui/
  canvas.*, font.*   software renderer (anti-aliased shapes, Inter), no GPU, no dependencies
  ui.*               immediate-mode controls: buttons, dropdowns, value boxes, bars, menus, prompts
  editor.*           CLAP gui extension, zoom, frame clock, still/dynamic layers
  common.*           the parts every TwoStorms window shares: top bar, presets menu, A/B, settings
                     popover, meters, Act card and menus
  theme.h            colors
  platform_x11.cpp, platform_win32.cpp
src/plugins/compressor/
  comp_engine.*      the DSP: a port of @init / @block / @sample, no CLAP, no GUI
  comp_params.*      parameter table and factory presets
  comp_plugin.*      the Plugin subclass, the window, and the PluginKind
```

## What a plug-in gets without writing anything

- Stereo in/out (32- or 64-bit; the DSP always sees doubles), optional stereo sidechain.
- Parameters with gestures, edits from the window reaching the host, host automation in, value
  text both ways (what clap-validator checks), state save/load.
- A/B sets: Copy, Swap (Lvl Match), Bypass (Lvl Match) are in the top bar.
- Presets: the Presets menu with factory groups from your table, user presets saved per plug-in.
- Latency: return it from `latencyFromParams`; the base class debounces changes (0.25 s) and asks
  the host to restart.
- The Act: gain, meters, Auto, drive links, finale, the Gain Stage's remote control, the audio key
  for the Gate. Its parameters (IDs 1-42) are added with one call.
- REAPER: track, chain position, names and routing appear in shared memory for the Gain Stage and
  the windows.
- Flush-to-zero during `process`, a 50 ms main-thread timer, a window that costs nothing when closed.

## Step by step

### 1. Reference

Copy the JSFX into `tests/reference/<name>.jsfx`, and a `<name>_ref.jsfx` that `tools/jsfx_ref`
can run: ysfx has no `get_host_placement`, so give the copy a stub function returning zeros (see
`compressor_ref.jsfx`). The renderer takes `slider=value`, `t<frame>:slider=value` (a change at a
frame), `var:name=value` (set a variable after @init), `dump=a,b` (variables per block) and
`final=n` (print a slider at the end, for things like Set threshold).

### 2. Parameters (`<name>_params.cpp`)

- **The ID is the JSFX slider number.** Old preset strings keep working, the Gain Stage addresses
  parameters by the same numbers, and the comparison tests can pass the same arguments to both.
  1-42 belong to the Act; a plug-in's own sliders start at 50 (the Compressor uses 50-104).
- Start the table with `addActParams(t, true)`: the Act as one linked stereo pair, only "Output"
  (ID 10) host-visible.
- Flags: `PF_HOST` (the host sees and automates it), `PF_AB` (part of an A/B set), `PF_ENUM`
  (labels), `PF_INT`, `PF_BOOL` (On/Off), `PF_HIDDEN`. Window-only settings (meter range, zoom,
  help) are not `PF_HOST`.
- Give `unit` and, where the ends mean something else, `minText` / `maxText` ("off", "no limit"),
  so typing "1 s" into a millisecond field means 1000 ms and the host's text round-trips.
- **Never renumber a `PF_HOST` parameter once released.** REAPER stores automation and parameter
  links by ID.
- Factory presets are `{"Group/Name", "id=value id=value ..."}`, the same text the JSFX's preset
  strings use. Groups become submenus. The suite's genres: punk rock, pop punk, metal, rock, jazz,
  pop.

### 3. The engine (`<name>_engine.cpp`)

Port the plug-in's own @init / @slider / @block / @sample code into a class that knows nothing
about CLAP or the window. Give it a small host interface for parameter values (the Compressor's
`EngineHost`: `pv(id)`, `abv(id)` for the inactive A/B set, `setFromAudio(id, v)` for
`slider_automate`) and a `Display` struct of atomics for anything the window draws (meters,
histories, analyzer rings). Then see "EEL2 to C++" below; it is where ports go wrong.

### 4. The plug-in (`<name>_plugin.cpp`)

Derive from `Plugin` (and your engine's host interface) and fill in:

```cpp
void dspActivate(double sr, uint32_t maxFrames) override { eng.init(sr); }
void dspReset() override { eng.reset(); }
void dspProcess(const double *const *in, const double *const *sc, double *const *out, int n) override
{
    eng.block(n);                  // the plug-in's @block
    act.block(n);                  // the Act's @block (after the plug-in's, as in the JSFX)
    eng.samples(in, sc, out, n);   // the plug-in's @sample
    act.process(out, 2, n);        // the Act's @sample: output gain, meters, Auto
    eng.post(out, n);              // anything the JSFX did after the Act (output meters)
    act.audioKey(out[0], out[1], n);   // every plug-in: lets a Gate key from this output
}
int latencyFromParams(double sr) const override;   // 0 unless the zero-latency option is off
void stateSaveExtra(std::string &s) override;      // the JSFX's @serialize extras: "x key value\n"
void stateLoadExtra(const std::string &k, const std::string &v) override;
```

The Gain Stage's Set row for the plug-in uses three hooks: `hkRset(id)` returns the id if the row
may set that parameter remotely (0 otherwise), `hkTgStart()` starts the plug-in's own "Set ..."
action, `hkPublish(slot)` publishes the parameters the row shows (`slot.p[id]`).

Then the `PluginKind` (bottom of `comp_plugin.cpp`): descriptor (ID `com.twostorms.<name>`, name
"TwoStorms <Name>", features), table, factory presets, preset file name, window title,
`shm::KIND_POINT`, its `shm::SK_*` subkind, sidechain, A/B and the A/B parameter. Add
`&ts::<ns>::kind` to `s_kinds` in `src/entry.cpp` and the three source files to `TS_PLUGINS` in
`CMakeLists.txt`.

### 5. The window

A `gui::View`: `designSize` (the 100% size; the Compressor is 940 x 748), `frame(Ui&)` which draws
and handles input in one pass, and `statusHash()`, a hash of everything the still layer depends on
that is not a parameter (text that changes, modes). The editor redraws the still layer (panels,
labels, grids, curves) only when a parameter or the hash changes, and the dynamic layer (meters,
moving displays) every frame; put moving things in `ui.drawD` blocks. If something you draw moves
and isn't in the dynamic layer, it will freeze; if something static is in it, the window costs CPU.
`wantsFull()` returns true while a popover is open over moving parts.

Three more hooks, which the Tuner uses (`src/plugins/tuner`):

- `freeSize(baseW, baseH)`: a window of any size and shape. The design fits the window's shorter side
  (relative to the base) and grows along the other; `sizeChanged` gives the new design size (and the
  window's, to keep in the state), `savedSize` the one to open at. The shared zoom doesn't apply.
- `poll(now)`: called on every tick of the window's clock (about 60 a second) whether a frame is drawn
  or not; true asks for a frame. With `animating()` false, a window then draws only when something
  happened (the Tuner draws when a reading comes in, never in silence).
- `dynamicRegions(rects)`: where the moving parts are. A frame that doesn't redraw the still layer then
  restores, draws and presents only those rectangles (X11: `XShmPutImage` per rectangle; Win32: the
  blit through a clip region). Nothing moving may be drawn outside them.

Build it from `common.h`, top to bottom:

1. `topBar(...)`: logo, title, Presets, A/B (Copy, Swap, Bypass), gear. Same in every plug-in.
2. The plug-in's display, if it has one.
3. The parameter columns, with `ui.paramRow` (label, bar, value box). Dynamics columns are
   Threshold / Ratio / Knee | Attack / Hold / Release.
4. The row with Lookahead (and latency/phase mode, where there is one) and the "Set ..." button.
5. Input and Dry/wet. (The Compressor keeps its JSFX layout: Input / Range / Dry/wet as a third
   column, Character, Lookahead and Set threshold on the row below.)
6. `actCard(...)`: the Act (Output fader, meters, Auto, drive link).
7. Settings in the gear popover (`settingsBegin` / `settingsEnd`), so the window never changes size.

The suite's layout rules (yours, from the JSFX suite):

- The same sliders in the same order, similar functions in the same place and the same format,
  in every plug-in. When in doubt, the plug-ins Claude made are the reference.
- Presets at the top.
- Attack, Hold, Release in that order everywhere; Ratio, Knee and Hold are continuous.
- "Set ..." is one button that names itself ("Set threshold", "Set thresholds"), not a label and
  a separate Set button (the Gain Stage may keep its label + button rows).
- Lookahead, latency/phase mode and the Set button share a row, Set hugging the right edge.
- Input and Dry/wet share the row below, the Input bar longer than Dry/wet.
- A control that only applies in some states is hidden, not greyed out, where the layout allows,
  and hiding it never moves another row.
- Every plug-in keeps a zero-latency option; analyzers and the window add no latency and no CPU
  while closed.
- No sidechain trim on the EQ (setting the thresholds covers it).

Colors (`theme.h`): black panels; **orange** for what you act on (active buttons, bars, the
curve); **purple** for targets and references (the threshold line, Target GR, trigger ranges, drive
links); **green** for good (the safe zone of meters, OK states); amber and red only in meters and
warnings.

### 6. Tests

Copy `tests/engine_run.cpp` (renders the engine on a raw signal with the same argument syntax as
`jsfx_ref`) and add the plug-in's cases to a copy of `tests/compare.py`
(`python3 tests/compare.py build/linux/engine_run`). Aim for what the Compressor got: under 1e-11
against the JSFX on every case, at 44.1, 48 and 96 kHz and block sizes 64, 512 and 1000 (`SRS`,
`BLOCKS`, `ONLY` environment variables). Then, with `TS_PLUGIN=com.twostorms.<name>` to pick the
plug-in in the bundle:

```
build/linux/clap_host_test info   build/linux/TwoStorms.clap
build/linux/clap_host_test render build/linux/TwoStorms.clap in.f64 out.f64 48000 512 [n=v ...] [--f32] [--state]
build/linux/clap_host_test gui    build/linux/TwoStorms.clap out.png 500 [script ...]   # needs an X display (Xvfb)
build/linux/clap_host_test chain  build/linux/TwoStorms.clap
clap-validator validate build/linux/TwoStorms.clap
```

`render` must be bit-identical to `engine_run` in 64-bit, and `--state` must change nothing. GUI
script steps: `click:x,y`, `shiftclick:x,y`, `dclick:x,y`, `rclick:x,y`, `move:x,y`,
`drag:x0,y0,x1,y1`, `wheel:x,y,steps`, `key:text`, `enter`, `esc`, `wait:ms`, `shot:file.png`,
`set:id=v`, `print:id`. Coordinates are window pixels at the current zoom (shared by every
TwoStorms window through shared memory, so a test sets it first).

## EEL2 to C++

The numbers only match if the semantics do. What bit the Compressor port, or would have:

- **Comparisons are fuzzy.** `a == b` is `|a - b| < 0.00001`, `a != b` is the opposite, and a
  value is true when `|x| >= 0.00001`. Use `eq`, `ne` and `truth` from `eel.h` everywhere the JSFX
  compares, including `?:`, `&&`, `||`, `!` and `while`. Only `===` / `!==` are exact.
- **`x ^ y` is a power** (`epow`), not xor. `|`, `&`, `~` and `%` work on integers; convert
  explicitly and check edge cases (negative values) against ysfx.
- **Enum sliders are rounded with `floor(x + 0.5)`** (`iround`) and the JSFX usually clamps after.
- **Keep the order of operations.** `a*b*c` and `a*(b*c)` round differently. Don't simplify,
  don't hoist divisions into reciprocals unless the JSFX did, and keep `-ffp-contract=off` (set in
  CMake) so the compiler doesn't fuse multiply-adds. Same constants (`$pi` is `ts::PI`).
- **Variables start at 0 and persist.** Every JSFX variable is a member initialized to 0, and code
  that relies on a variable from the previous block keeps doing so. @init runs again on a sample-
  rate change and on reset.
- **@slider runs before the next @block** when a slider changed; port it into the start of
  `block()` (comparing with last values) or run it unconditionally if it is cheap and idempotent.
- **Local memory** (`buf[i]`) becomes `std::vector<double>` sized to what the JSFX allocates. EEL2
  returns 0 for reads outside memory; if the JSFX relies on that, guard the index.
- **Keep the JSFX's own denormal guards** (`flush21`: `abs(x) < 1e-21 ? x = 0`). FTZ is on anyway,
  but the guard changes the numbers, so leave it.
- **Built-ins:** `srate`, `samplesblock`, `spl0`/`spl1` (main), `spl2`/`spl3` (sidechain),
  `slider_automate` → `setFromAudio`, `pdc_delay` → `latencyFromParams`, `slider_show` → the window
  hides the control, `gmem[]` → shared memory, `get_host_placement` → `Plugin::placement()`.
- **Bugs:** if the port differs from the JSFX, find out which one is wrong before "fixing" the
  port. The Compressor's saturation spike was the JSFX's; the fix is a flag (`satFix`, off with
  `TS_EXACT=1` in tests) so the exact comparison still runs, and the JSFX got the same fix.

## Shared memory

One named block per user: `/dev/shm/TwoStormsCLAP.<layout>.<uid>` on Linux, `Local\TwoStormsCLAP.<layout>`
on Windows (layout 3 now). It replaces the JSFX suite's `gmem` "TwoStormsGS" with a typed struct (`shm.h`):

- **Header:** magic, layout version, the shared window zoom, and `presetv` (bumped when a user
  preset is saved or deleted; open windows re-read their list).
- **1024 slots**, one per live instance, claimed with a compare-and-swap on `owner`. The owner
  writes its status, meters (8 channels), Auto state and published parameters `p[1..41]` (plus
  whatever `hkPublish` adds). A heartbeat (`heart`, `hbc`) tells others it is alive.
- **Command ring** per slot (16 entries; spinlock for the many writers, read by the owner's audio
  thread): `C_SET`, `C_ADDGAIN` (drive links), `C_AUTO`, `C_STOP`, `C_CANCEL`, `C_RESETHOLD`,
  `C_TGSTART` / `C_TGSTOP` (Set rows). Nobody writes another instance's slot directly; they send.
- **Names** (track name, previous plug-in, drive-linked parameter) are written by the owner's main
  thread from REAPER, under a sequence lock.
- **64 audio lanes** for the Gate's key: 32768 float samples per channel each. The Tuner reads them
  too, from its window (any track: it asks through `areq` every frame and reads the lane up to
  `astart`, the start of the block being written), at the rate in `srate`. A Tuner listening with
  Mute on (and "Mute all tracks heard") writes the time into `tmute` of each plug-in it listens to; while that is fresh, `Act::audioKey` silences the plug-in's
  output after writing the lane (10 ms fades), so the Tuner still hears it.

Every field is an 8-byte atomic, so nothing on an audio thread ever locks or allocates. If the
layout changes, bump `LAYOUT`: the name changes with it, so old and new builds running at the
same time don't read each other's memory.

**The CLAP suite and the JSFX suite can't share this.** REAPER doesn't expose gmem to native
plug-ins. Until the Gain Stage and Act are ported, a CLAP plug-in only sees other CLAP plug-ins. A
bridge is possible (a ReaScript reading both sides through functions the CLAP registers with
REAPER), but it would be a stopgap; porting the Gain Stage early is the cleaner fix.

## What the next plug-ins need that isn't built yet

| Plug-in | Missing piece |
|---|---|
| Act (standalone) | Up to 8 channels: `PluginKind` needs a channel count and the audio-ports extension a matching port; `addActParams(t, false)` already gives the channel options. |
| Gain Stage | `KIND_MASTER`. The window-side helpers in `act_view.*` (lists, names, chain order, Set rows, finale) are ported; it needs its own window listing every slot by subkind (it must recognize every TwoStorms plug-in), and Set-threshold rows that call `C_TGSTART` / `C_SET`: the Compressor's, the Multiband's (all bands at once, a Target GR per band) and the Wah's. |
| Gate | The audio-key **reader**: count `areq` up once per block while keying, then read `alane`, `astart`, `alen` and the lane's samples, checking `aseq` before and after. Every plug-in already writes. |
| IR Loader | Loading files on the main thread and handing them to the audio thread (a second `Spsc` of pointers), and a file dialog: inside REAPER, `GetUserFileNameForRead` through `ReaperBridge`. |
| Delay, Chorus, Tremolo, Wah | Tempo sync: pass `clap_process::transport` (tempo, position, playing) through to `dspProcess`. |
| EQ, Multiband | Move the Compressor's FFT analyzer (`Fft4096` and the columns drawing in `comp_plugin.cpp`) into `src/gui` once a second plug-in uses it. |
| Saturator | The Compressor's saturation (with the ADAA re-anchoring) is in `comp_engine.cpp`; port the Saturator's own JSFX code rather than reusing it, so it matches its JSFX. |

## Builds

See the README. The released Linux binary is the Zig build (glibc 2.27 and up, X11 loaded at
runtime, only `clap_entry` exported); the Windows binary is the MinGW build (static runtime, no
DLLs). Both come from the same sources, and the tests compare them: the Windows build's output under
Wine matches Linux to -243 dB.

## What the JSFX does that the port keeps (and what it fixes)

Kept, so the sound matches the JSFX sample for sample:
- Saturator, Tape Head: `TH_G3` is 0 (in EEL2 `-10 ^ x` is `(-10) ^ x`, not a number, and EEL turns it into 0).
- Saturator: the Global "Input" modulation target only moves its marker (the audio reads the slider itself).
- Delay: the diffusion allpasses' phases are all 0 (EEL2's `%` is an integer remainder); the modulation
  envelope `MEN` is flushed to 0 under 1e-21 each block.
- EQ: the `zl`/`zr` state flush never fires.
- EEL2 precedence: `/` binds tighter than `*` (`a*b/c` is `a*(b/c)`); the ports are parenthesised to match.

Fixed in the CLAP versions:
- Compressor: an abrupt saturation change could make one sample spike (up to +37 dBFS); the JSFX 1.0.1 has the
  same fix.
- IR Loader: an array one entry too short (33 entries in 32) meant a new Tail trim level never rebuilt the
  filter ("unclick and reclick").
- Multiband: a band switched Off still got its Character's saturation; Set thresholds simulated every band
  with the last band's curve.
- Delay: the echo map seeded series routing from both lines (a display quirk).
