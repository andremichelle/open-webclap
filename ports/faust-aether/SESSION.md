---
title: "Porting Aether (faust-aether) to WebCLAP in open-webclap"
subtitle: "Annotated session transcript, with reasoning"
author: "Mistral Vibe — session log for Björn"
date: "7 November 2026"
lang: en
---

# About this document

A reconstruction of a single working session on the `open-webclap` repository (the fork
`synth-is/open-webclap`, branch `main`), following the Oxo Synth2 session. It records what was asked, what I
reasoned, what I did, and where I was wrong and how I found out.

---

## The question: JUCE kit, or the Synth2 pattern — and where should the port live?

Aether is a Faust project that *also* exports to the web (faust-rs → wasm + its own JS page on GitHub Pages)
and to JUCE (`faust2juce` → AU/VST3/standalone with a custom editor). Two questions came up: does the JUCE
support make the move to WebCLAP easier, and should a WebCLAP build be contributed *to faust-aether* or to
*open-webclap*?

Reading both projects settled it:

- open-webclap's JUCE kit (`modules/juce_webclap`) is for plugins *written* in JUCE. Aether's JUCE plugin is
  *generated* by `faust2juce`, which needs a Faust `master-dev` source checkout, and its build downloads JUCE 8
  — two wasm modules for a generated processor plus a ~1.8 kloc custom editor. Heavy and fragile for a port.
- The DSP itself is what matters, and Faust's C++ backend is already wasm-clean: upstream's own `make web`
  compiles the same `aether.dsp` to wasm. So the plain-CLAP route (Helm, coopiano, Oxo Synth2 — engine behind
  a small wrapper, editor as an HTML page) fits: no Faust source checkout needed, only the compiler binary
  (2.88.0 release dmg, installed to `~/faust`), and **zero upstream patches**.
- A WebCLAP port belongs in open-webclap (`ports/faust-aether/`, pinned in `scripts/fetch-deps.py`), because
  that is where the test host, the site, CI, the pack/license tooling and the PR process live — exactly the
  Synth2 pattern. faust-aether's own web export is a different artifact (faust-rs wasm + a bespoke JS host on
  GitHub Pages, not CLAP); a WebCLAP bundle makes Aether a device *inside* openDAW Studio. Optionally, upstream
  could later link to the WebCLAP port from its README — a courtesy, not the port itself.

## How the port is built

`gen.py` runs the pinned Faust compiler on the pinned checkout (`external/faust-aether`, commit
`28f262c1…`): `aether.dsp` → class `AetherVoice`, `effects.dsp` → class `AetherFX` (single precision, like the
published web build; `-cn` keeps the class names distinct so both compile in one TU). It also reads the
compiler's JSON metadata and writes `params.inc` (a C++ table of all 96 widgets — the 83 voice leaves minus
`freq`/`gain`/`gate`, plus the 16 bus widgets; the bus's copy of `bpm` is driven by the voice id) and
`patches.json` (upstream's six presets, short names resolved to full addresses, titles/tags/descriptions from
upstream's `web/app.js` — same MIT project).

`aether_clap.cpp` is the host the DSP expects: 8 voices, note allocation with one-block fade on steal (ported
from upstream's `web/aether-engine.js`), the stereo bus computed once per block, parameters written straight
into the generated classes' widget zones (collected by a `UI` implementation that rebuilds the Faust
addresses), a 396-byte `AETH` state, sleep in silence, and the binary page protocol (same shape as the Oxo
Synth2 one). `ui/index.html` adapts upstream's `web/` page — preset library, tabs, matrix table, step grid —
onto that protocol.

## Where I was wrong

- **Typo `CLAP_PLUGIN_FEATURE_SYNTHIZER`** (missing the E) cost a confusing compile error; the preprocessor
  output showed `CLAP_PLUGIN_FEATURE_INSTRUMENT` expanding but not the other — that pinpointed it.
- **A long detour on "the note never releases in wasm."** I instrumented the wrapper (per-voice key/flags/
  gate/level in the activity message) and chased heap-layout-dependent behavior across native and wasm builds,
  split and single runs. The wrapper was correct all along (a native driver and a minimal wasm driver both
  released fine); the actual bug was in my `check.mjs`: the `NOTE_OFF` events carried no `key`, so `noteOff(0)`
  matched nothing. The sustained "3e-2" was the held note.
- **Same class of harness bug twice more:** `run()` with an absolute `until` below the current position runs
  zero blocks (the MIDI test silently did nothing), and `host.write(...)` followed by `run([])` loses the event
  (`write([])` resets the count). Fixed by passing events through `run` and using a `flush()` helper (as oxo's
  check does) for out-events.
- **Page startup race:** `applyPreset("nebula")` ran on the first snapshot, before `patches.json` had
  loaded, so no preset was selected; and `Object.keys(patches).entries()` destructured as `[key, index]`
  instead of `[index, key]`, crashing `buildPage` with "reading 'title'". Both fixed; the page now selects
  Nebula once the table, the presets and the snapshot have all arrived.
- The wrapper's hello handler cleared the "send all values" flags before the values loop, so the first poll
  after hello had no values message — fixed by not clearing them in the snapshot branch.

## Verification

- `check.mjs` (node CLAP host, WASI-only imports asserted): descriptor `org.faust.aether` v1.0.0; 96 params
  with ranges/defaults/modules, menu texts ("LFO 1"…"Macro 2"), checkbox and filter-type texts; 396-byte state
  round-trip; a note sounds, release + 12 s tail end in silence, then `CLAP_PROCESS_SLEEP`; cutoff changes the
  sound measurably; MIDI dialect; the full page protocol (hello/describe/snapshot/values, set→host event,
  host→poll values, patch, hover, panic). DSP: **0.52 ms per 128-sample block = 19% of realtime with an 8-note
  chord** in node wasm.
- Test host (headless Chromium, `127.0.0.1:8123`): editor renders (6 preset cards, 6 tabs, 12/5+9-route/16/
  4+16-step/96 controls per tab), no element outside the 1024×640 stage, slider drag updates readout, preset
  switch to Acid/Glass works, host keyboard note → voices 1/8, meter moves, no console errors.
- Site (`localhost:8130/faust-aether?local`): home lists Aether, page loads from the packed bundle, Nebula
  selected, computer-key note plays (voices 1/8), no errors. Live Pages verified after push (see the git
  log / CI run).

## Files

| Path | What |
|---|---|
| `ports/faust-aether/gen.py` | pinned Faust compiler → `voice_gen.cpp`/`fx_gen.cpp`, `params.inc`, `patches.json` |
| `ports/faust-aether/aether_clap.cpp` | the CLAP wrapper: polyphonic host, params, state, page protocol |
| `ports/faust-aether/CMakeLists.txt` | emcmake build of `module.wasm` (160 KB) |
| `ports/faust-aether/ui/index.html` | editor page, adapted from upstream `web/` |
| `ports/faust-aether/check.mjs` | node CLAP host test suite |
| `ports/faust-aether/{README.md,NOTICES.md.in,SESSION.md}` | port doc, license notices, this log |
| `scripts/build-faust-aether.sh` | fetch → gen → build → check → license → pack → site copy |
| `scripts/fetch-deps.py` | the `faust-aether` pin (sletz/faust-aether @ `28f262c1…`) |
| `site/src/plugins.ts`, `README.md` | site entry (`bundled:`) and table row |
