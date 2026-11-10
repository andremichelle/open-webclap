# OpenRB

The TB-303 and the four effects of [OpenRB-338](https://github.com/pingw33n/openrb-338) (pingw33n, GPL-3.0-only),
the bit-exact reimplementation of ReBirth RB-338 2.01's engine, as five WebCLAP plugins in one module:

| Plugin | CLAP id | Kind |
|---|---|---|
| OpenRB 303 | `studio.opendaw.openrb.bass` | instrument |
| OpenRB Distortion | `studio.opendaw.openrb.distortion` | audio effect |
| OpenRB PCF | `studio.opendaw.openrb.pcf` | audio effect |
| OpenRB Compressor | `studio.opendaw.openrb.compressor` | audio effect |
| OpenRB Delay | `studio.opendaw.openrb.delay` | audio effect |

The TR-808 and TR-909 are not ported: they play ReBirth's own drum samples, which are Propellerhead's material.

## How

- `engine/`: upstream's `orb-engine` crate (fetched into `external/openrb-338`), unchanged, behind a small C API
  (`engine/src/lib.rs`), built as a static library for `wasm32-unknown-emscripten`.
- `plugin/openrb.cpp`: the CLAP side on `modules/opendaw-clap/opendaw_page.h`. The engine runs at its own 44.1 kHz in
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
  each 8th in patterns with their shuffle on. Sound: 2.0 or Vintage (song-wide in the original). Gain (-24 to +6 dB, 0 the engine's own level) is the port's:
  the original's accents pass 0 dB before its mixer turns them down.
- **PCF**: its 55 patterns follow the host's beat position while it plays and run on at its tempo while it stops.
- **Delay**: steps of 16ths or 8th triplets at the host's tempo; Send is the mixer's send knob (the dry signal
  passes), each instance has its own line.
- **Compressor**: linked stereo, as on the original's master.

## Build

```sh
rustup target add wasm32-unknown-emscripten
scripts/build-openrb.sh   # build/openrb/openrb.wclap.tar.gz, then node ports/openrb/check.mjs (20 checks)
```

Rust's std carries wasm target features newer than emsdk 3.1.51's wasm-opt knows, so `build.sh` strips the
`target_features` section from the archive and names the features at the link (bulk memory, saturating float to
int, wasm exceptions).
