# Elements (faust-elements) port

[Elements](https://github.com/sletz/faust-elements) (sletz, MIT) — the Faust model of Mutable Instruments'
Elements eurorack module (Emilie Gillet, 2014, MIT) — as a WebCLAP bundle. Plain CLAP, no JUCE, and **no
upstream patches**: `gen.py` runs the pinned **faust-rs** compiler on this port's `src/voice.dsp` /
`src/body.dsp` (the voice and the effect bus of upstream's `dsp/elements_poly.dsp`, verbatim) into two
self-contained C++ classes (`ElementsVoice`, `ElementsBody`), and `elements_clap.cpp` is the polyphonic host
around them, the same split upstream's web AudioWorklet uses:

- `module.wasm`: the synth as a CLAP instrument (`org.faust.elements`). The model runs at **32 kHz** — every
  table and delay of the firmware is computed for the module's codec rate — so the DSPs are init'ed at 32000
  and the wrapper resamples to the host rate with a Kaiser-windowed sinc (the browser does the same for the
  web version's 32 kHz context). Internally audio advances in 16-frame chunks (the model's block). 8 voices
  (the web default): allocation, restrike (one low-gate block per note), deferred release, culling
  (1 s released / 3 s held below 1e-5), stealing (same key → free → oldest released → oldest), divergence
  guards (voice 1e3, body 1e6 → `instanceClear`) and body sleep are a direct port of
  `web/elements-processor.js`. 21 parameters (ids 0..20: the voice's exciter/resonator/unit/performance
  controls, then the body's space/diffusion/reverb_lp), as their native values with min/max/default/step/unit
  from the `.dsp` metadata; the resonator model is a stepped enum (Modal/String/Strings). Pitch bend / tuning
  note expressions add to the modulation (pitch offset) parameter. The state is a fixed 96-byte blob
  (`ELEM` + version + count + 21 floats). The plugin returns `CLAP_PROCESS_SLEEP` after half a second of
  silence. WASI-only imports; `module.wasm` is ~885 KB.
- `ui/index.html`: the editor page, adapted from upstream's own `web/` page (same MIT project): the 24 web
  presets as a library (`ui/patches.json`, built by `gen.py` from upstream's `web/controls.js` via
  `extract-presets.mjs`), one tab per panel group, a playable on-screen keyboard (page note messages), output
  meter and voice count, an **Orbit** tab (the vendored faust-orbit-ui component with `ui/orbit.json`: every
  control a dot, the presets as its projected library, plus the "Where the sound is" mini-atlas beside it with
  the current sound as a diamond), and an **Atlas** tab rendering upstream's `web/atlas.json` (the sound atlas
  of the web page — see ATLAS.md for how the features and the projection are made): 2,024 sounds laid out by
  their measured descriptors, click a dot to load its patch and hear it.

Build: `scripts/build-faust-elements.sh` (needs Emscripten, a Rust toolchain for the one-time faust-rs build
cached in `external/faust-rs`, and the Faust 2.88.0 release at `~/faust` for its standard libraries —
`elements.lib` uses faust-rs's `ondemand` domains, which upstream Faust 2.88 does not have; fetches the pinned
sources, runs `gen.py`, builds, runs `check.mjs`, packs `build/faust-elements/faust-elements.wclap.tar.gz`,
and copies it to `site/public/bundled/` so the site serves the bundle itself). Test in the test host:
`python3 scripts/serve.py .` then
`http://127.0.0.1:8123/test-host/?bundle=/build/faust-elements/web/faust-elements.wclap`. On a Pages
deployment (e.g. a fork's), `/faust-elements` runs from that bundled copy; the openDAW-hosted catalogue route
needs an admin upload and its CORS allows only the studio's own origin.

## Why faust-rs, not the Faust compiler

`dsp/elements.lib` is written for faust-rs: its block-clocked `ondemand` domains (the C++ computes once per
16-sample block; the model freezes those parts between) have no equivalent in the upstream Faust 2.88
compiler (`undefined symbol: ondemand`). faust-rs (grame-cncm/faust-rs, pinned at the commit faust-elements'
own results were measured with, `22999fe5`) compiles the same sources to self-contained C++ (`-lang cpp`),
so the port keeps the sample-accurate model and stays a plain-CLAP build — no JUCE, no faust2juce, no wasm
DSP wrapper. The standard libraries (`basics.lib` etc.) still come from the official Faust release.

## Keeping up with faust-elements

The port tracks upstream by its pinned commit, one line in `scripts/fetch-deps.py`
(`("faust-elements", "sletz/faust-elements", "<commit>", …)`), plus the faust-rs commit pinned in
`scripts/build-faust-elements.sh` (`FAUST_RS_COMMIT`). To take an upstream improvement:

1. Bump the commit in the `DEPS` entry and update the commit named in `NOTICES.md.in`. Run
   `python3 scripts/fetch-deps.py` — the fetch filter pulls `dsp/elements.lib`, `dsp/elements_tables.lib`,
   `web/controls.js`, `web/atlas.json`, `LICENSE`, `README.md`. New or changed presets in `web/controls.js`
   are picked up by `extract-presets.mjs` → `gen.py` automatically; if the atlas changed, regenerate
   `web/atlas.json` upstream (`make web-atlas`) and refetch.
2. Clean build and test: `rm -rf build/faust-elements && scripts/build-faust-elements.sh` (runs `check.mjs`:
   the parameter contract and menu texts, the 96-byte state round-trip, notes/MIDI/tuning, the 32 kHz → host
   resampler's pitch, release and sleep, the page protocol, DSP timing at 29% realtime for an 8-note chord).
   If the widget set changed, `gen.py` re-derives the parameter table; if the wrapper's assumptions broke
   (control addresses, 3-output voice / 3-input body), fix `elements_clap.cpp`.
3. If upstream itself needs a change to compile, put it in a small `patches/faust-elements-<name>.patch` per
   the repo's convention — and ideally propose it upstream. Today there are none.

Upstream's own gates (`make test`, `make web-check`, `make web-bounds`, `make web-fuzz`) cover the model
sample-by-sample against the firmware; this repo's `check.mjs` covers the wrapper contract.

Known gaps, in priority order:

- No `opendaw:device-panel` page of its own yet (openDAW shows its generic parameter panel, all 21 parameters
  grouped by module; the window has the full editor).
- No Web MIDI button in the page: notes come from the host, or the page's on-screen keyboard. Upstream's web
  page has Web MIDI and a hold pedal.
- The Atlas tab is a static projection (click-to-load; no live measurement of the current sound, which
  upstream's page does in a Web Worker), and the timbre orbit (descriptors-as-dots) is not ported; the Orbit
  tab is (faust-orbit-ui, presets as its library). See ATLAS.md.
- Polyphony is fixed at 8 (the web version offers 1..16); a compile-time constant in `elements_clap.cpp`.
- The state saves the 21 parameters only, not the DSP internals (same as the Aether port); the reverb tail is
  declared infinite (`CLAP_EXT_TAIL` = UINT32_MAX).
