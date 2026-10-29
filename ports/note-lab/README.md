# Note Lab WebCLAP

The reference note effect (CLAP feature `note-effect`): notes in, notes out, no audio. It is the test plugin for MIDI
effect hosting and the template for porting one. Plain CLAP in C++, no JUCE, MIT.

```sh
scripts/build-note-lab.sh   # builds build/note-lab/web/note-lab.wclap, packs it, runs check.mjs
```

## Layout

```
build.sh        builds module.wasm and copies the page and LICENSE into $OUT (CLAP headers in $CLAP_INCLUDE)
plugin/         notelab.cpp, the whole plugin
page/           index.html, the device panel (?panel) and the window in one page
check.mjs       a minimal CLAP host in node: note streams per mode, state bytes per edit, page bytes per message
```

## The plugin

- One input and one output note port (CLAP and MIDI dialects), no audio ports. Non-note MIDI passes through
  unchanged, CC 123 releases everything.
- Modes: Thru, Harmony (plus a third and a fifth), Echo (repeats at a delay with falling velocity), Hold (note offs
  dropped, the next note ends the held ones). Transpose applies to every mode, keys out of range are dropped.
- Every note is sample exact. A note off releases exactly what its note on started, whatever the parameters say by
  then. A key that sounds already is re-attacked (off, then on) and ends with its last source, so the stream never
  holds a key twice.
- A mode change releases every sounding note at its sample. Reset, stop and deactivate only forget: the host
  releases what it got.
- Eight presets A to H, picked by the Preset parameter. Picking one reports the values it brings as parameter
  events. Mode, Delay, Repeats and Transpose edit the picked preset.

## State

72 bytes: `NLAB`, version, preset, two reserved bytes, then eight 8-byte presets (mode, repeats, transpose, reserved,
delay as float32 little endian). The layout never moves, so an edit changes at most four bytes and a host that
stores ranged edits records tiny history steps.

## The page

- `?panel` is openDAW's device panel (352 x 176, feature `opendaw:device-panel`, extension
  `opendaw.device-panel/1`): the flower, the presets and Big View, which asks the host to open the window
  (`clap.gui request_show`). Without the query it is the window, 640 x 400, every host shows it.
- The flower has one petal per pitch class: the notes going out bloom, the notes coming in are the dots inside.
- Traffic: the page says hello once and gets a snapshot (80 bytes). It polls at most every 33 ms, the plugin
  answers only when something changed, with a mask, the changed values (8 bytes each) and the note events
  (4 bytes each).
- Every control reports `clap.param-hovered`, ctrl+click (macOS's context click) is ignored.
