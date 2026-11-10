# OpenRB

The TB-303, TR-808, TR-909 and the four effects of [OpenRB-338](https://github.com/pingw33n/openrb-338) (pingw33n,
GPL-3.0-only), the bit-exact reimplementation of ReBirth RB-338 2.01's engine, as seven WebCLAP plugins, each in a
bundle of its own (`build/openrb/<bundle>.wclap.tar.gz`):

| Plugin | Bundle | CLAP id | Kind |
|---|---|---|---|
| OpenRB 303 | `openrb-bass` | `studio.opendaw.openrb.bass` | instrument |
| OpenRB Distortion | `openrb-distortion` | `studio.opendaw.openrb.distortion` | audio effect |
| OpenRB PCF | `openrb-pcf` | `studio.opendaw.openrb.pcf` | audio effect |
| OpenRB Compressor | `openrb-compressor` | `studio.opendaw.openrb.compressor` | audio effect |
| OpenRB Delay | `openrb-delay` | `studio.opendaw.openrb.delay` | audio effect |
| OpenRB 808 | `openrb-808` | `studio.opendaw.openrb.tr808` | instrument |
| OpenRB 909 | `openrb-909` | `studio.opendaw.openrb.tr909` | instrument |

The TR-808 and TR-909 played ReBirth's own drum samples, which are Reason Studios' (Propellerhead's) recordings. This
port ships none of them. It ships factory kits made of free recordings plus a few synthesised layers instead
([kits/README.md](kits/README.md): sources, licences, the CC BY credit for the 909 samples), and the user can bring
ReBirth's (see Behaviour).

## How

- `engine/`: upstream's `orb-engine` crate (fetched into `external/openrb-338`), unchanged, behind a small C API
  (`engine/src/lib.rs`), built as a static library for `wasm32-unknown-emscripten`.
- `plugin/openrb.cpp`: the CLAP side on `modules/opendaw-clap/opendaw_page.h`, compiled once per device
  (`OPENRB_KIND`): each module's factory holds its one plugin, the 808's and 909's their own factory kit. The engine runs at its own 44.1 kHz in
  64-frame blocks, as the original does; the host's audio is resampled to it and back (`plugin/resampler.h`, the
  effects report the latency).
- `page/index.html`: the device panels. Every device is its panel, there is no window (no `clap.gui`): the 303's
  pattern editor is its panel's screen. Each a single row of controls without frames or titles: scaled knobs with an orange
  pointer, LED switches and dark screens (only `pluginHost` and `fit` come from `modules/opendaw-page`). `?preview` shows every
  panel with its measured width.

## Behaviour

- **303**: bank A holds eight factory patterns written for this port (`factory_patterns` in `plugin/openrb.cpp`),
  B to D start empty. On the pattern screen a click sets a step's note where it lands, a click on its note makes it a
  rest, a drag draws; the ACC and SLD rows toggle. Play: Pattern (the default) plays the patterns, Play: Notes takes notes (velocity above 100 accents, a legato note slides, MIDI notes too; C2 is the
  lowest C at the middle tune). Play: Pattern plays its 32 patterns (A1..D8, 16 steps of note, up, down, accent,
  slide, rest) on the host's transport: a new pattern starts at the next bar line, Shuffle delays the second 16th of
  each 8th. Sound: 2.0 or Vintage (song-wide in the original). Gain (-24 to +6 dB, 0 the engine's own level) is the port's:
  the original's accents pass 0 dB before its mixer turns them down.
- **808, 909**: play their factory kits (embedded in the module, saved in the state as a reference of 4 bytes per
  slot). Drop `ReBirth.dll` on the panel (the free download of the ReBirth
  Museum; the plugin reads its 46 AIFF resources with the engine's own `SampleSet::from_dll`), or sample files named
  as their slots (808: `BD`, `SD1`..`SD5`, `N1`..`N4`, `CB`, `CH`, `CL`, `CP`, `CY`, `HC`, `HT`, `LC`, `LT`, `MA`,
  `MC`, `MT`, `OH`, `RS`; 909: `bd1`..`bd4`, `sd1`..`sd5`, `sdn1`..`sdn3`, `lt`, `mt`, `ht`, `rs`, `hc`, `chh`, `ohh`,
  `ccy`, `rcy`; any format the browser decodes, taken as 44.1 kHz mono).
  Samples the user brings live in the plugin's state (only the machine's own slots). Mods (`.rbm`) are not read: upstream's mod
  crate carries ReBirth's standard kit and part of its decryption key. Notes on General MIDI's drum keys play the
  instruments in both modes (velocity above 100 accents, on the 909 its strong hit); Play: Pattern (the default) runs
  the 32 patterns on the transport, bank A with four factory patterns each. The step grid on the panel: a click
  toggles a step (909: off, on, strong; Alt its flam), dragging paints along the row. Upstream's `trigger` is made
  public for the notes (`patches/openrb-338-note-trigger.patch`).
- **Shuffle** (303, 808, 909) is one knob: every pattern shuffles, at 0 nothing moves (the original's per-pattern
  switch is always on).
- **PCF**: its 55 patterns follow the host's beat position while it plays and run on at its tempo while it stops.
- **Delay**: steps of 16ths or 8th triplets at the host's tempo; Send is the mixer's send knob (the dry signal
  passes), each instance has its own line.
- **Compressor**: linked stereo, as on the original's master.
- **Parameters** in the units the panels show: percent for the device's 0..127 knobs (a step of 100/127), the 303's
  Tune in semitones, Pan -63..63, the PCF's waves 1..55, the distortion's Shape 0 (Clip) and its curves 1..32.
- **Tail and sleep**: the effects report their tail (the delay's repeats until -60 dB, endless at full feedback).
  Every device stops its engine after 50 ms of silence, without input, notes or a running pattern (the delay waits
  for its line as well), and wakes on the next note or input.

## Build

```sh
rustup target add wasm32-unknown-emscripten
scripts/build-openrb.sh   # the seven build/openrb/<bundle>.wclap.tar.gz, then node ports/openrb/check.mjs (31 checks)
```

Rust's std carries wasm target features newer than emsdk 3.1.51's wasm-opt knows, so `build.sh` strips the
`target_features` section from the archive and names the features at the link (bulk memory, saturating float to
int, wasm exceptions).
