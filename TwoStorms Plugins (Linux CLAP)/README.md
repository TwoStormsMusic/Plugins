# TwoStorms CLAP

The TwoStorms suite as CLAP plug-ins for Linux and Windows, one file per plug-in:

| | |
|---|---|
| `TwoStorms Act.clap` | the Act on its own (gain, meters, Auto, drive links, finale) |
| `TwoStorms Gain Stage.clap` | the whole track's gain staging and the Set row |
| `TwoStorms Compressor.clap` | compressor (Clean VCA, Opto, FET, Vari-mu), trigger ranges, lookahead |
| `TwoStorms EQ.clap` | 16-band EQ, dynamic bands, zero latency / linear / natural phase |
| `TwoStorms Multiband.clap` | up to 4 bands, draggable thresholds in the analyzer |
| `TwoStorms IR Loader.clap` | two IRs, any sample rate (resampled), normalized, true zero latency |
| `TwoStorms Saturator.clap` | up to 6 bands, 30 styles, oversampling |
| `TwoStorms Delay.clap` | three lines (parallel / series / linked), pitch / reverse, character, ducking, reverb, modulation |
| `TwoStorms Chorus.clap` | chorus, ensemble, flanger, vibrato, through-zero; Standard or High quality |
| `TwoStorms Tremolo.clap` | Tremolo / Auto-pan / Harmonic, LFO and 1-4 step grids |
| `TwoStorms Wah.clap` | up to 3 wahs, MIDI pedal / envelope / transient / LFO, Voice (talk box) |
| `TwoStorms Defitcher.clap` | three-band de-esser |
| `TwoStorms Gate.clap` | gate with hysteresis, hold, lookahead, external or TwoStorms key |
| `TwoStorms Tuner.clap` | chromatic tuner (guitar, bass, voice) for this track and any Gain Stages; Mute; any window size |

## Install

Copy the files you want (any of them, they don't depend on each other):

| | |
|---|---|
| Linux | `~/.clap/` (or `/usr/lib/clap/`) |
| Windows | `C:\Program Files\Common Files\CLAP\` |

Remove an older `TwoStorms.clap` (the single file that held every plug-in) if it is there: it has the
same plug-in IDs. Then in REAPER: *Options > Preferences > Plug-ins > CLAP > Re-scan*. The plug-ins are
listed as **CLAP: TwoStorms Compressor (TwoStorms)** and so on. Sidechains are the second input pair
(channels 3-4, as in the JSFX). The plug-ins find each other through shared memory, so the Gain Stage,
Match, Reference, drive links and the Gate's TwoStorms key work across the separate files.

The Linux build runs on glibc 2.27 or newer (Ubuntu 18.04, Debian 10, Fedora 28 and later) and needs
only libX11 and libXext, which it loads when a window opens. The Windows build needs nothing besides
Windows itself.

## Compared with the JSFX

- **The sound:** every engine is a line-by-line port and was rendered against the original JSFX
  (through ysfx) over a matrix of settings, sample rates and block sizes (`tests/cases/*.py`); the
  largest difference anywhere is around 1e-11 (about -215 dB). The JSFX bugs that changed what you hear
  or see are fixed in the CLAP versions (the Compressor's saturation spike, the IR Loader's tail trim,
  the Multiband's saturation on an Off band and its Set thresholds); a few harmless quirks were kept so
  the sound matches (`docs/PORTING.md`, "What the JSFX does that the port keeps").
- **Beyond the JSFX** (CLAP only): the Chorus's High quality (4x oversampled drive and BBD, the LFO
  every sample); the Tremolo's three modes with one to four grids that play together or take turns (each
  group with its own number of steps, up to 64), a level on every step, the Gap, 1/64 and 1/64T, a pitch
  per grid (Tremolo mode; the shifter's 4096-sample frame is the only latency, and only while a grid is
  pitched), grids that line up step for step, and its tempo from the project, tapped or typed; the IR
  Loader's Normalize (one IR or two at 0 dB play as loud as the input, K-weighted as LUFS; the levels
  act on top; on by default) and its waveforms drawn once as an 8x supersampled picture (linear, L
  above, R below; both IRs on one scale as they play, the louder at the full height; the wheel zooms in on
  time, Ctrl + wheel on the height, a double-click shows it all) and what Align did, in milliseconds; the EQ's dynamic
  bands' full range, shaded; the Wah's Voice as real formants, in series (a vocal tract) or in parallel,
  its three bands switched on and off one by one (double-click a band's tab, or On), and the Pedal button; the Delay's third line, parallel / series / linked between each pair of lines, a
  reverse that plays every repeat backwards, dry and wet levels, ducking for the repeats and the reverb
  apart, and its settings as groups in a list that scrolls; clearer analyzers everywhere; factory
  presets on every plug-in with presets in Instruments (guitar, bass, vocals, drums, keys and more),
  Buses (instrument bus, mixbus, master), Creative and Weird.
- **The Tuner** (CLAP only) listens to this track's input and any TwoStorms plug-ins in the project, Gain
  Stages above all, ticked in its Listen to list (as many as you like; it hears the loudest), so it can
  sit on a track of its own with no audio. Mute silences its output and, with "Mute all tracks heard" on
  (Settings), every plug-in it listens to (which still feed the Tuner, and whose windows say "MUTED BY
  TUNER" under their name); they play again a quarter second after the Tuner lets go (Mute off, the Tuner
  removed or bypassed). Mute,
  the reference, range, tolerance and display are host parameters, so a footswitch can learn them. The
  pitch is the McLeod method with a second pass low-passed just above the fundamental (a stiff string's
  sharp upper partials don't pull the reading), a fading note flattened out, and the period measured over
  as many periods as the window holds: within 0.1 cent on plucked strings from a five-string's low B up
  (a third of a cent on that B at 176.4 or 192 kHz), at the fundamental on stiff strings. It runs in the window, about 30 times a second (0.3 ms each), and
  the window draws only what moves (the needle, the strobe, the numbers) and nothing at all in silence;
  with the window closed it costs nothing. The window takes any size and shape: the design fits the
  shorter side and grows along the other.
- **A bypassed plug-in keeps its place.** Its audio stops but its main thread keeps its slot in shared
  memory, so no other instance (a new one, or a copy carrying the same ID) can take it over; finales,
  chain order and keys stay put. (`clap_host_test bypasskeep` checks it.) A plug-in in a bypassed FX
  chain counts as off for the finale.
- **No helper script.** Track names, chain order, drive links, routing checks and user presets come
  straight from REAPER.
- **The CLAP and JSFX suites can't see each other**: REAPER doesn't give native plug-ins JSFX shared
  memory (gmem).
- User presets live in `<REAPER resource path>/Data/TwoStorms/Presets/` (outside REAPER:
  `~/.config/TwoStorms/Presets/` or `%APPDATA%\TwoStorms\Presets\`).
- A latency change (a lookahead, a linear-phase EQ, a pitched Tremolo grid) makes REAPER restart the
  plug-in to take the new latency, as with any plug-in whose latency changes.
- Every window draws its still parts only when they change, and its meters and analyzers cost nothing
  while it is closed. (A window can also say where its moving parts are: then only those are redrawn
  and sent to the screen, which the Tuner does.)

## Building

Linux (with the tests):

```
cmake -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build/linux
```

The Linux release (portable to old glibc), with Zig (`pip install ziglang`):

```
cmake -B build/zig -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/zig-linux.cmake -DTS_ZIG=ON -DTS_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
ninja -C build/zig
```

Windows, cross-compiled on Linux (`apt install g++-mingw-w64-x86-64-posix`):

```
cmake -B build/win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DTS_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
ninja -C build/win
```

On Windows itself, CMake with Visual Studio 2019 or newer builds the same tree.

## Tests

- `tools/jsfx_ref` renders a JSFX through [ysfx](https://github.com/jpcima/ysfx) (patched to allow 256
  sliders); `build/linux/engine_run` renders the port; `python3 tests/compare.py build/linux/engine_run`
  runs both over a matrix of settings (`TS_EXACT=1` turns the saturation fix off so the comparison
  is exact).
- `ninja -C build/linux` also makes `TwoStorms.clap` (every plug-in in one file), which the tests load.
- `build/linux/clap_host_test` loads a bundle like a host: `info`, `render` (also through state
  save and load), `gui` (opens the window on an X display, scripted clicks, screenshots) and `chain`
  (two instances: Match and a drive link through shared memory). `tests/win_host_test.cpp` is the
  Windows twin (runs under Wine).
- `tests/tremolo_new.py`, `tests/wah_voice.py`, `tests/chorus_hq.py`, `tests/ir_norm.py` and
  `tests/delay_new.py` check what goes beyond the JSFX (no reference): the Tremolo's gap, groups, levels
  and pitch; the Wah's formants (series and parallel); the Chorus's High quality; the IR Loader's
  Normalize (pink noise comes out within 0.5 dB of the input's loudness, one IR or two); the Delay's
  Line C, series and linked lines, dry and wet levels, the reverb ducking and the reverse (a rising chirp
  comes back falling, every repeat, with no gaps). The IR Loader's comparison cases run with Normalize
  off; the Delay's with Line C off, the JSFX's dry/wet balance as the two levels, and no reverse.
- `tests/tuner_pitch.py` runs the Tuner's pitch detector (`build/linux/tuner_test`) on made-up notes:
  sines at 44.1, 48 and 96 kHz, plucked bass and guitar strings, a weak fundamental, stiff strings, a
  low B at 192 kHz, a sung note with vibrato, noise and silence. `tests/tuner_clap.py` checks the
  plug-in: untouched audio, Mute's fades, state, and (`clap_host_test tunermute`) two Gain Stages muted
  through the Tuner and let go, or left playing with "Mute all tracks heard" off. `clap_host_test gui` takes `TS_SIG=<file>` (another signal),
  `TS_EXTRA=<ids>` and `TS_MAIN_SILENT=1` (the windowed Tuner on a silent track, listening to an extra
  Gain Stage), and the steps `resize:w,h`, `linkid:<param>[,<extra>]`, `xset:<id>=<v>`, `xlinkid:<param>` and
  `extrapeak`.
- [clap-validator](https://github.com/free-audio/clap-validator): every test passes for every file. Its
  occasional warning ("took 2x longer to process denormals") is a false positive: its timed loop
  includes generating the subnormal numbers, and measured directly (`clap_host_test render` on
  subnormal input) every plug-in runs at its normal speed.

## Licenses

TwoStorms code: yours. Bundled: the CLAP headers (MIT), stb_truetype and stb_image_write (public
domain), Inter (SIL Open Font License 1.1, `resources/fonts/LICENSE-Inter.txt`), X11 headers (MIT/X11).
