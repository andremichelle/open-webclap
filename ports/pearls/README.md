# Pearls WebCLAP

A granular texture effect after Mutable Instruments' Beads (Emilie Gillet, MIT): it records its input into a buffer
and plays it back as grains, a delay or a mix of both, through a reverb. Plain CLAP in C++, no JUCE, MIT. Pearls is
not affiliated with or endorsed by Mutable Instruments.

```sh
scripts/build-pearls.sh   # builds build/pearls/web/pearls.wclap, packs it, runs check.mjs, copies it to the site
```

## Layout

```
build.sh        builds module.wasm from plugin/ and Beads' DSP, copies the page and LICENSE into $OUT
                (CLAP headers in $CLAP_INCLUDE, Beads and stmlib in $EURORACK)
plugin/         pearls.cpp, the whole plugin around Beads' Processor
page/           index.html, the panel (636 x 176, openDAW's device panel at 1:1, zoomed in the window);
                ?preview shows it without a plugin
check.mjs       a minimal CLAP host in node: latency, resampling, grains, freeze, sync, state and page traffic
```

`scripts/fetch-deps.py` pins Beads (`pichenettes/eurorack`, only `beads/dsp` and its tables) and stmlib at the
eurorack submodule's commit into `external/eurorack`. `patches/stmlib-wasm.patch` makes stmlib pick its portable code
over ARM assembly under emscripten.

## The DSP

Beads' code runs unchanged and as on the module:

- At the rate its quality gives the codec: dirty tape 24 kHz (8 bit, wow and flutter), clean tape 48 kHz with the
  grains at 24 kHz (12 bit), lo-fi digital 32 kHz (12 bit), HQ 48 kHz (16 bit). The host's audio is resampled there
  and back with a windowed sinc (8 zero crossings, cut below the lower Nyquist); equal rates pass through.
- In the module's blocks, a 4 kHz block rate (12, 8 or 6 samples), with the pots' smoothing (one pole, 0.05 a block).
- The input: a gain instead of the module's automatic one, then the tape settings' soft clipping (always on dirty
  tape, rising with the gain on clean tape), hard clipping on digital.
- Latency: what the resampling and a block need, so the queue between them never runs dry, plus the dirty tape's
  average wow and flutter (50 samples at 24 kHz; the tape moves it by up to 73 either way). A quality or record
  change restarts the module (its buffer empties, as on the hardware) behind a 10 ms fade and announces the new
  latency.
- Cost: 18 to 30 µs per 128 frames, 35 to 46 µs in delay mode (node, Apple silicon), under 2 % of real time. SIMD
  brings nothing measurable, so the build goes without.

## Parameters

| Parameter | Range | On the module |
|---|---|---|
| Time, Size, Shape | 0..1 | the knobs; Size at its end is delay mode |
| Density | 0..1 | left of noon regular grains, right of it random ones, faster towards the ends |
| Pitch | ±24 semitones | the knob (without its snapping to intervals); Quantize rounds to semitones |
| Time, Pitch, Size, Shape random | -1..1 | the attenuverters with nothing patched: a random value per grain (positive), or a random amount of it per grain (negative); in delay mode a random walk |
| Feedback, Dry/Wet, Reverb | 0..1 | the mix knobs |
| Input, Output | ±24 dB | the input level (the module sets it itself), an output gain |
| Quality | dirty tape, clean tape, lo-fi digital, HQ | the recording media |
| Record | stereo, mono | mono sums both channels into a buffer twice as long (as with only the left input patched) |
| Freeze | off, on | stops recording, the grains keep reading the buffer |
| Sync | off, 1/32 .. 1 bar | the host's tempo as a clock in the SEED jack while the transport plays: Density divides it (left of noon) or skips pulses (right of noon), the delay locks to it |

Left out: the CV inputs, the wavetable synth (the module's sound with nothing patched; Pearls is an audio effect), and
Seed with its latch (Pearls is always latched, the module's state at power-up, so Density or Sync runs the grains).

## State

`PRL1`, the count, then per parameter its id (FNV-1a of its key) and value (f64), little endian. It loads by id:
unknown ids are skipped, missing ones keep their defaults.

## The page

- Left to right as the signal flows: the input, quality and record; the grain knobs with their random dials (the value
  line shows the random amount while one is hovered), Freeze and Sync; feedback and reverb; dry/wet and output. The
  name along the bottom.
- Traffic: the page polls once a frame; the plugin answers with the values (f64) and the input and output peaks since
  the last poll. A drag is one gesture, so one undo step in openDAW. The page reports the parameter under the pointer
  (`clap.param-hovered`), so openDAW's right-click menu and double-click value entry work on it; the page has no wheel
  and no double-click of its own.
