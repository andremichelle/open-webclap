# How faust-elements' sound atlas (and Orbit) are made — reference

Source: [sletz/faust-elements](https://github.com/sletz/faust-elements) @ `651cf94a` (pinned in
`scripts/fetch-deps.py`). Files: `scripts/gen_atlas.mjs` (the generator), `web/describe.js` (the measurements),
`web/atlas.js` (the page), `web/atlas.json` (the committed data, ~414 KB), `web/orbit.js` +
`web/vendor/faust-orbit-ui/` (the Orbit tab). The WebCLAP editor in `ports/faust-elements/ui/` renders
`atlas.json` in its Atlas tab and embeds the orbit component in its Orbit tab.

## 1. The corpus: what gets measured

`make web-atlas` = `node scripts/gen_atlas.mjs [COUNT=2000] [SEED=1]`:

- **2,000 random patches**: every panel control drawn uniform in its declared range (the resonator `model`
  a whole number 0/1/2), exactly as the Orbit's *Random* button draws them (a seeded 32-bit LCG, so an atlas is
  reproducible). The note controls (freq/gain/gate) and the pitch offset are excluded.
- **Plus the 24 web presets** (`web/controls.js` PRESETS) as labelled landmarks.
- Each patch is **rendered offline** with the page's wasm DSPs (voice + body at 32 kHz), played as the page
  plays: **A3 (MIDI 57, 220 Hz), velocity 0.7, gate held 1 s of a 2 s render**, one voice into the body, with
  the page's divergence guard (a voice beyond 1e3 or non-finite ends the render; the patch is dropped, as are
  patches quieter than −60 dBFS). Result: 2,024 points kept.
- Patches are stored with their control values rounded to 3 decimals (the 4th moves some sounds by several dB).

`web/atlas.json` layout: `note` (the measured note), `axes` (descriptor names), `controls` (the control keys,
the order of each point's `v`), `projection` {axes, mean, sd} (the standardization), `components` (the two PCs:
variance share + loadings per descriptor), `points` [{preset?, c? (decay extrapolated), pc: [x, y], d: [8
descriptors], v: [20 control values]}].

## 2. The features: 8 audio descriptors (`web/describe.js`)

Measured on the stereo output (aux left, main right) while the note is held unless stated:

| descriptor | definition |
|---|---|
| `brightness` | spectral centroid, Hz |
| `noisiness` | spectral flatness, dB — 0 for white noise, ≈ −75 for a pure tone; bins more than 100 dB below the strongest count at that level (a stated floor, not numerical noise) |
| `harmonicity` | share of the energy within a tenth of the 220 Hz fundamental's harmonics (≈ 0.2 flat spectrum, 1 harmonic) |
| `attack` | time to reach 90 % of the loudest 10 ms frame, s (at least 10 ms) |
| `sustain` | level of the last 200 ms of the hold against the loudest frame, dB |
| `decay` | time from the loudest frame down 30 dB, s; when the render ends first, extrapolated from the least-squares slope of its last 500 ms (`c: 1` marks it), capped at 30 s |
| `loudness` | loudest 10 ms frame, dBFS |
| `width` | side against mid energy, (L−R) vs (L+R), dB |

`LOG_AXES` = {brightness, attack, decay} are read on a log scale.

## 3. The projection: PCA on the standardized descriptors

1. Standardize each descriptor: `(x − mean) / sd` of the atlas, log-transformed first for the log axes. The
   mean/sd are stored in `atlas.json`'s `projection`, so **new sounds can be placed on the same plane** (the
   page re-renders and re-measures the current patch in a Web Worker after every change and shows it as a
   diamond on the atlas and on the small map beside the orbit).
2. Drop `loudness` — it follows the level, not the timbre. 7 axes remain: brightness, noisiness, harmonicity,
   attack, sustain, decay, width.
3. PCA (covariance eigendecomposition of the standardized descriptors) → the plane is the **first two
   principal components**:
   - **PC1 ≈ 28.5 % of the variance** — the *envelope* axis: loadings ≈ −0.51 sustain, −0.48 decay, −0.45 attack,
     −0.37 brightness, −0.40 noisiness;
   - **PC2 ≈ 25.3 %** — the *spectrum* axis: −0.55 brightness, −0.53 noisiness, +0.30 harmonicity, +0.28 attack.
   (Loadings as committed in `atlas.json`; signs are arbitrary.)
4. Rendering: dot at `pc`, **colour = resonator model** (modal/string/strings), presets drawn larger and
   labelled; hover shows the eight measures; click loads the patch (its 20 control values) and plays the
   measured note. Wheel/pinch zooms about the pointer (over an axis's ticks, that axis alone), drag pans,
   double-click fits. Any descriptor can be chosen as an axis instead of the PCs.

Cross-check upstream does: levels measured on eight sounds re-rendered by faustprobe agree within 0.02 dB.

## 4. The Orbit tab (control-space explorer, `web/orbit.js` + faust-orbit-ui 0.4.1)

- Every panel control (except note/velocity/gate and the pitch offset) is a **dot on an orbit**; drag one dot or
  the centre to move all; *Random* scatters them (the same distribution as the atlas's random patches).
- Its **library (the "calque")** starts with the 24 presets, **projected on a plane by PCA of the control
  values**; click a disc to recall a preset, **drag the cross between discs to morph** (Shepard
  interpolation), double-click keeps the current sound. Additions/renames/deletions persist in localStorage.
- The resonator `model` (a whole number) is rounded while the orbit interpolates.
- The **timbre orbit** (upstream page only): a second orbit whose dots are the *descriptors* as percentiles
  among the atlas's sounds; moving them asks for a sound — the page loads the nearest atlas sound (of the
  current model when one is nearly as near) and refines by a local search over ~40 measured renders. On the
  atlas: dashed ring = asked, diamond = obtained. (Blending several neighbours' controls was tried and
  dropped: the controls do not mix linearly into the timbre.)

## 5. In the WebCLAP port

- Atlas tab: `ports/faust-elements/ui/index.html` renders `web/atlas.json` (2,024 points, PC plane, model
  colours, preset labels, descriptor tooltips, zoom/pan, click loads the patch as one `patch` message and
  plays A3). The current sound is shown as a diamond at the **atlas point nearest to the current controls**
  (each point carries its 20 control values) — upstream instead re-renders and re-measures the patch in a Web
  Worker and projects the descriptors; that needs the wasm in the page, which the WebCLAP editor does not have.
- Orbit tab: the vendored faust-orbit-ui component with `ui/orbit.json` (the descriptor gen.py derives from
  the compiler's JSON: voice + body groups, note controls and pitch offset excluded), the 24 presets as its
  library (persisted in localStorage); a moved dot sends the same `set` message as a slider. Beside it, the
  compact **"Where the sound is"** mini-atlas of upstream's Orbit tab (same atlas data, no labels, same
  diamond, click-to-load, wheel zoom / drag pan).
- Not ported: the timbre orbit, the phone/PWA tab, Web MIDI, the hold pedal, and the worker-based live
  measurement of the current sound.
