---
title: "Porting Elements (faust-elements) to WebCLAP in open-webclap"
subtitle: "Annotated session transcript, with reasoning — including how faust-elements' Orbit tab and sound atlas work"
author: "Mistral Vibe — session log for Björn"
date: "7 November 2026"
lang: en
---

# About this document

A reconstruction of a single working session on the `open-webclap` repository (the fork
`synth-is/open-webclap`, branch `main`), following the Oxo Synth2 and Aether sessions. It records what was
asked, what I reasoned, what I did, and where I was wrong and how I found out.

---

## The question: what can be ported from faust-elements, and how does its web Orbit/atlas work?

[faust-elements](https://github.com/sletz/faust-elements) (local clone at `WCLAP/faust-elements`, upstream
`sletz/faust-elements`) is not a JUCE plugin: it is a **Faust model of Mutable Instruments' Elements
module**, checked sample-by-sample against the firmware (`make test`: −72 dB of the peak or better in single
precision on every scenario), with a web version (`make web`) that compiles the voice and the body of
`dsp/elements_poly.dsp` to wasm and runs them in an AudioWorklet at 32 kHz. So the Synth2/Aether pattern
applies directly: compile the DSP to C++/wasm, wrap it in plain CLAP, adapt the web page as the editor.

The two web DSPs are `el.voice` (0 in, 3 out: raw/center/side) and `el.body` (3 in, 2 out: main/aux — the
mix, soft limiter and Griesinger reverb), and `web/elements-processor.js` is the polyphonic host around
them. The port keeps that split exactly: `src/voice.dsp` and `src/body.dsp` are `elements_poly.dsp`'s
process expressions verbatim (same `declare name "elements_poly"`, so all control addresses are upstream's,
`/elements_poly/exciter/bow`, …).

**The one real obstacle: `ondemand`.** The library is written for **faust-rs**, not the upstream Faust
compiler: everything the C++ computes once per 16-sample block lives in `ondemand` domains, and Faust 2.88
answers `undefined symbol: ondemand`. There is no standard-Faust spelling for "evaluate this stateful block
only on the block clock" (freezing a recursion's delays is not expressible), so the port builds the pinned
**faust-rs** (`grame-cncm/faust-rs` @ `22999fe5…`, the commit faust-elements' own results were measured
with) once with cargo and uses its `-lang cpp` backend, which emits *self-contained* C++ (it brings its own
`dsp`/`UI` types; the includer provides them, like upstream's `tools/render.cpp` does). Standard libraries
(`basics.lib`, …) still come from the official Faust 2.88 release at `~/faust`. faust-rs compiles the voice
in ~2 s; single precision is its default.

**The 32 kHz rate** is the model's, baked into every table and delay, so the wrapper always inits the DSPs at
32000 and resamples the host stream from 32 kHz with a Kaiser-windowed sinc (16 taps/side; the web version
lets the browser resample its 32 kHz AudioContext instead). Internally the audio advances in 16-frame
chunks, the model's block, so gate/control changes take effect at block boundaries exactly as on the module.

## The side question: how the Orbit tab and the sound atlas are made

Both live in upstream's `web/` and both are now partly in the port's editor:

- **Orbit tab** (`web/orbit.js` + vendored [faust-orbit-ui](https://github.com/orlarey/faust-orbit-ui) 0.4.1,
  Yann Orlarey, MIT): every panel control (but note/velocity/pitch offset) is a dot on an orbit — drag one
  dot or the centre to move them all, "Random" scatters them. Its *library* holds the 24 presets projected on
  a plane by **PCA of the controls**; click a disc to recall a preset, drag the cross between discs to morph
  (Shepard interpolation), double-click to keep the current sound. The model (a whole number) is rounded
  while the orbit interpolates. The port does not include the orbit (faust-orbit-ui is a separate component);
  the editor has the Atlas tab instead.
- **Atlas tab** (`web/atlas.js`, data `web/atlas.json` from `make web-atlas` / `scripts/gen_atlas.mjs`): a
  **sound atlas** — sounds laid out by what they sound like, not by their controls. 2,000 random patches
  (each control drawn uniform in its range, model a whole number — the same distribution as the orbit's
  Random) plus the 24 presets are each **rendered offline**: one voice into the body at 32 kHz, played as
  A3 (MIDI 57, 220 Hz) at velocity 0.7, gate held 1 s of a 2 s render. Each render is measured by
  `web/describe.js` with **eight audio descriptors**:
  - *brightness* — spectral centroid (Hz);
  - *noisiness* — spectral flatness in dB (0 = white noise, ≈ −75 = pure tone; bins >100 dB below the peak
    clamped at that floor);
  - *harmonicity* — share of the energy within a tenth of the 220 Hz fundamental's harmonics;
  - *attack* — time to 90 % of the loudest 10 ms frame;
  - *sustain* — level of the last 200 ms of the hold vs the loudest frame, dB;
  - *decay* — time from the loudest frame down 30 dB (extrapolated by least-squares slope of the last 500 ms
    when the render ends first, as for 65 % of sounds; capped at 30 s);
  - *loudness* — loudest 10 ms frame, dBFS;
  - *width* — side vs mid energy ((L−R) vs (L+R)), dB.
  The **projection is PCA**: the descriptors are standardized (log for brightness/attack/decay), loudness is
  dropped (it follows level, not timbre), and the plane is the **first two principal components** —
  PC1 ≈ 28 % of the variance, the *envelope* axis (sustain/decay/attack loadings); PC2 ≈ 25 %, the *spectrum*
  axis (brightness/noisiness). `atlas.json` stores the standardization (mean/sd per axis) and the loadings,
  so the page can place *new* sounds (it re-renders and measures the current patch in a Web Worker after every
  change, and shows it as a diamond). Dot colour = resonator model; presets are labelled; hover shows the
  measures; click loads the patch and plays the measured note.
- **Timbre orbit** (upstream only): a second orbit whose dots are the *descriptors* as percentiles among the
  atlas's sounds; the page loads the nearest atlas sound and refines by local search.

The port's Atlas tab renders `web/atlas.json` directly: the 2,024 points at their PC coordinates, coloured by
model, presets labelled, hover tooltip with the eight descriptors, wheel-zoom/drag-pan, and click loads the
point's 20 control values as a patch (and plays A3). No live measurement of the current sound (that needs the
wasm + a worker in the page).

## How the port is built

`scripts/build-faust-elements.sh`: fetch pinned deps → build faust-rs once (cached in `external/faust-rs`,
`cargo build --release -p compiler`) → `extract-presets.mjs` (upstream's `web/controls.js` PRESETS → JSON,
with the comment above each preset as its tag) → `gen.py` (faust-rs → `voice_gen.cpp`/`body_gen.cpp`, plus
`params.inc` from the compiler's JSON: 21 params, model as a Modal/String/Strings menu, and
`patches.json`) → emcmake build of `module.wasm` → `check.mjs` → license/pack → site copy.

`elements_clap.cpp` is the host: 8 voices with the allocation/restrike/deferred-release/culling/stealing/
guards/body-sleep ported line-by-line from `web/elements-processor.js` (computeChunk is its `process()` at
16-frame quanta; outputs aux-left/main-right as `elements.cc`); voices' 3 outputs sum into the body's 3
inputs, body computed once per chunk; 21 params written into the generated classes' widget zones; pitch
bend / tuning expressions add to the modulation param; 96-byte `ELEM` state; the binary page protocol (same
shape as Aether's, plus note on/off messages for the on-screen keyboard).

Both generated files compile in **one TU**: faust-rs prefixes its tables/SIG classes with the `-cn` class
name, and only the `faust_wrap_*` helpers are unguarded, so the wrapper `#define`-renames them per include.

## Where I was wrong

- **faust-rs needs the standard libraries on its import path**: its repo's `libraries/` only holds its own
  additions; `basics.lib` & co. come from the Faust release (`-I ~/faust/share/faust`).
- **The resampler's cutoff ratio was inverted**: `0.5 * min(1, 32000/hostRate)` lowpassed at 10 kHz when
  upsampling 32k→48k. Correct is `0.5 * min(1, hostRate/32000)` cycles per internal sample. The pitch test
  (spectral centroid of a bowed A4 at 48 kHz vs 32 kHz host rate: 462.9 vs 465.1 Hz) caught the class of
  bug, not the inversion itself — the inversion was found by reading the formula.
- **Two harness bugs in `check.mjs`**: the note-expression struct was written at wrong offsets
  (`expression_id` is at 16, `key` at 28), and I first used event type 6 for NOTE_EXPRESSION — it is **4**
  (6 is PARAM_MOD, silently ignored), so the +12 st bend "didn't work" until the type was fixed; then the
  centroid comparison itself hardcoded 32 kHz bin frequencies for the 48 kHz render (fixed by passing the
  rate), and used the decaying default strike (fixed by bowing with an ADSR envelope and skipping the
  attack).
- **Atlas pointer coordinates**: `offsetX/offsetY` are already relative to the canvas; subtracting
  `getBoundingClientRect().left` again made hover/click miss every dot (found by dispatching synthetic
  pointer events in the page and reading `atlasHover`).

## Verification

- `check.mjs` (node CLAP host, WASI-only imports asserted): descriptor `org.faust.elements` v1.0.0; 21 params
  with the model menu ("Modal"/"String"/"Strings") and "+1.50 st"/"Hz" texts; 96-byte state round-trip; a bowed
  note sounds and its centroid matches at 48 kHz and 32 kHz host rates (resampler keeps pitch); release →
  silence → `CLAP_PROCESS_SLEEP`; +12 st tuning expression doubles the centroid (463 → 869 Hz); string model
  sounds; MIDI dialect; the full page protocol incl. page note on/off. DSP: **0.76 ms per 128-sample block =
  29% of realtime with an 8-note modal chord** in node wasm (upstream's native cost for one modal voice is
  11.2 ms/s ≈ 1.1% of a core, so 8 voices ≈ 9% natively).
- Test host (headless Chromium, `127.0.0.1:8123/test-host/?bundle=/build/faust-elements/web/faust-elements.wclap`):
  editor renders (24 preset cards, 6 tabs, keyboard), slider drag works, on-screen key plays (voices 1/8,
  meter 97%), preset "Bowed glass" loads, Atlas tab draws all 2,024 points with labels/legend, click on the
  "glass" dot loads it and plays A3 (voices 1/8, meter 75%), hover shows the descriptors, no console errors.
- Site (`localhost:8130/faust-elements?local`): home lists Elements, the page loads from the packed bundle,
  24 cards / 6 tabs, host-key note plays (voices 1/8), atlas 2,024 points, no errors.

## Files

| Path | What |
|---|---|
| `ports/faust-elements/src/{voice,body}.dsp` | the voice and body of upstream's `elements_poly.dsp`, verbatim |
| `ports/faust-elements/gen.py` | pinned faust-rs → `voice_gen.cpp`/`body_gen.cpp`, `params.inc`, `patches.json` |
| `ports/faust-elements/extract-presets.mjs` | upstream `web/controls.js` PRESETS → JSON for gen.py |
| `ports/faust-elements/elements_clap.cpp` | the CLAP wrapper: 32 kHz engine + resampler, voice manager, params, state, page protocol |
| `ports/faust-elements/CMakeLists.txt` | emcmake build of `module.wasm` (~885 KB) |
| `ports/faust-elements/ui/index.html` | editor page (presets, tabs, keyboard, atlas), adapted from upstream `web/` |
| `ports/faust-elements/check.mjs` | node CLAP host test suite |
| `ports/faust-elements/{README.md,NOTICES.md.in,SESSION.md}` | port doc, license notices, this log |
| `scripts/build-faust-elements.sh` | faust-rs pin/build, fetch → gen → build → check → license → pack → site copy |
| `scripts/fetch-deps.py` | the `faust-elements` pin (sletz/faust-elements @ `651cf94a…`) |
| `site/src/plugins.ts`, `README.md`, `site/public/bundled/faust-elements.wclap.tar.gz` | site entry (`bundled:`), table row, the self-served bundle |
