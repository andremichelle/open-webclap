# Hive WebCLAP

A note sequencer on a hexagonal grid inside a hexagon (CLAP feature `note-effect`). Spawn tiles start cursors on a
pulse or on incoming notes, cursors walk the tiles' arrows, tiles play notes when a cursor enters, splits fork the
cursors, sinks swallow them. Plain CLAP in C++, no JUCE, CC0-1.0.

```sh
scripts/build-hive.sh   # builds build/hive/web/hive.wclap, packs it, runs check.mjs, copies it to the site
```

## Layout

```
build.sh        builds module.wasm and copies the page and LICENSE into $OUT (CLAP headers in $CLAP_INCLUDE)
plugin/         hive.cpp, the whole plugin
page/           index.html, the device panel (?panel) and the window in one page
check.mjs       a minimal CLAP host in node with a transport: timing, durations, rate, a groove's shifted ranges,
                note triggers, pattern changes, stop and jumps, repeatability, state and page traffic
```

## The plugin

- Tiles: Path (an arrow, or keep the cursor's heading), Spawn (on a pulse, on notes, or both; a lifetime in steps or
  while the key is held), Sink, Turn (relative to the heading), Split (any set of directions), Random (seeded by
  position, so it repeats). Any tile but a sink can play a note on entry: pitch (auto from a Tonnetz layout, or
  fixed), velocity, repeats, chance. Each tile sets how long a cursor stays on it, 1/32 to 1 bar.
- Patterns: eight, each with its size (2 to 8 tiles on an edge), edge (wrap, bounce, die), root and scale. A pattern
  change waits for the next bar while playing.
- Parameters: Pattern, Rate (1/8 to 8/1, divides every tile's duration), Gate, Transpose.
- Time: the host's beat position, 384 ticks a quarter. Ranges shifted a little (a groove before it) carry on from the
  last tick run, a jump of more than a beat starts the cursors over. Incoming notes start cursors on the next 1/16,
  transposed by the key minus C4.

## State

21,712 bytes: `HIVE`, version, the parameters, then eight patterns, each a header (8 bytes: size, edge, root, scale)
and 169 tiles of 16 bytes. The layout never moves, so a tile edit changes at most 16 bytes. A drag on the grid
sends its tiles live and tells the host once at the end, so a drawn path is one undo step.

## The page

- `?panel` is openDAW's device panel (352 x 176): the grid with the cursors, the patterns and Edit, which asks the
  host to open the window. Without the query it is the window, 1000 x 640.
- Traffic: a snapshot on hello (21,728 bytes); then, polled at most every 33 ms, deltas with the changed
  parameters, tile and header edits (20 bytes each), while playing the tiles that played, the cursors (4 bytes each)
  and where cursors reached the edge (4 bytes each). Every page gets every message, so the panel and the window stay
  in sync.
