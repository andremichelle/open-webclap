# Aether (faust-aether) port

[Aether](https://github.com/sletz/faust-aether) (sletz, MIT), a hybrid polyphonic synthesizer written entirely in
Faust, as a WebCLAP bundle. Plain CLAP, no JUCE, and **no upstream patches**: `gen.py` runs the pinned Faust
compiler (2.88.0) on upstream's `aether.dsp` / `effects.dsp` — single precision, like the published web build —
into two C++ classes (`AetherVoice`, `AetherFX`), and `aether_clap.cpp` is the polyphonic host around them, the
same split upstream's web, JUCE and test hosts use:

- `module.wasm`: the synth as a CLAP instrument (`org.faust.aether`). 8 voices. 96 parameters (ids 0..95,
  gen.py's table order: the voice's groups, then the effect bus's), as their native Faust values with the
  min/max/default/step, log scale, unit and menu names from the `.dsp`'s metadata; menus and checkboxes are
  stepped, `value_to_text`/`text_to_value` use the menu names. The state is a fixed 396-byte blob (`AETH` +
  version + count + 96 floats). Notes (CLAP and MIDI dialects) are sample-accurate: a note reuses its key's
  voice, else a free one, else the quietest released, else the oldest; a stolen voice fades over one block.
  Released voices sleep after 200 ms of silence and the plugin returns `CLAP_PROCESS_SLEEP` after half a second
  of silence (the 12 s effect tail resets it). WASI-only imports.
- `ui/index.html`: the editor page, adapted from upstream's own `web/` page (same MIT project): preset library,
  tabs over the parameter groups, the 8-route modulation matrix, the 16-step sequencer, output meter. It talks
  to the plugin over binary messages (see the top of `aether_clap.cpp`) and loads the six presets from
  `ui/patches.json` (built by `gen.py` from upstream's `presets/patches.json`, short names resolved to full
  Faust addresses, presentation strings from upstream's `web/app.js`).

Build: `scripts/build-faust-aether.sh` (needs Emscripten and the Faust compiler — `FAUST=/path/to/faust`, on
`PATH`, or installed to `~/faust`; fetches the pinned sources, runs `gen.py`, builds, runs `check.mjs`, packs
`build/faust-aether/faust-aether.wclap.tar.gz`, and copies it to `site/public/bundled/` so the site serves the
bundle itself). Test in the test host: `python3 scripts/serve.py .` then
`http://127.0.0.1:8123/test-host/?bundle=/build/faust-aether/web/faust-aether.wclap`. On a Pages deployment
(e.g. a fork's), `/faust-aether` runs from that bundled copy; the openDAW-hosted catalogue route needs an
admin upload and its CORS allows only the studio's own origin.

## Why not the JUCE kit

faust-aether also exports a JUCE plugin (`make juce`, via `faust2juce`), but that path needs a Faust
`master-dev` source checkout and downloads JUCE 8 to build the generated processor plus its large custom
editor — two wasm modules through `modules/juce_webclap`. The DSP itself is what matters here, and Faust's C++
backend is already wasm-clean (upstream's own web build compiles the same `.dsp` to wasm), so the plain-CLAP
route — the one Helm, coopiano and Oxo Synth2 use — is both lighter and needs no Faust source checkout, only
the compiler binary. The JUCE plugin remains upstream's; nothing is contributed back to it.

## Keeping up with faust-aether

The port tracks upstream by its pinned commit, one line in `scripts/fetch-deps.py`
(`("faust-aether", "sletz/faust-aether", "<commit>", …)`). To take an upstream improvement:

1. Bump the commit in that `DEPS` entry and update the commit named in `NOTICES.md.in`. Run
   `python3 scripts/fetch-deps.py` — the changed dep is refetched (the `.fetched` stamp differs). The fetch
   filter pulls `aether.dsp`, `effects.dsp`, `audition.dsp`, `presets/patches.json`, `web/`, `LICENSE`,
   `README.md`. New presets in `patches.json` are picked up by `gen.py` automatically; the descriptor's
   version comes from `declare version` in the pinned `aether.dsp`, so no other version string needs editing.
2. Clean build and test: `rm -rf build/faust-aether && scripts/build-faust-aether.sh` (runs `check.mjs`: the
   parameter contract and menu texts, the 396-byte state round-trip, note/MIDI behavior, release and sleep,
   the page protocol, DSP timing). If the `.dsp`'s widget set changed, `gen.py` re-derives the parameter table
   and `ui/patches.json`; if the wrapper's assumptions broke (voice count, `freq`/`gain`/`gate` addresses), fix
   `aether_clap.cpp`, and if the web page's structure changed, re-adapt `ui/index.html` from the pinned `web/`.
3. If upstream itself needs a change to compile as wasm, put it in a small `patches/faust-aether-<name>.patch`
   per the repo's convention (add the prefix to `PATCH_TARGETS`) — and ideally propose it upstream so the pin
   eventually needs no patch. Today there are none: the compiler output builds unchanged.

Upstream's own gates (`make test`, `make web-check`) cover the DSP against `faustprobe` and the wasm build;
this repo's `check.mjs` covers the wrapper contract. When intentionally bumping the pin, listen for sound
changes — that is upstream's business, not a port regression.

Known gaps, in priority order:

- No `opendaw:device-panel` page of its own yet (openDAW shows its generic parameter panel, all 96 parameters
  grouped by module; the window has the full editor).
- No Web MIDI button: notes come from the host (the site offers keyboard/MIDI). Upstream's web page has one.
- The scope/telemetry of upstream's web page is reduced to the output meter and voice count (the page gets
  an activity message on polls); no waveform display.
- Polyphony is fixed at 8 (the web version offers 4/8/12); it is a compile-time constant in `aether_clap.cpp`.
