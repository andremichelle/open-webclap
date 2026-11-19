# Prism WebCLAP

[Prism](https://github.com/Boof2015/prism) by Boof2015, GPL-3.0: the eight analyzers of its DAW plugins as
WebCLAP audio effects in one bundle (Spectrum, Oscilloscope, Vectorscope, Spectrogram, VU Meter, Loudness Meter,
Waveform, Waterfall). The audio passes through unchanged. Each is its device panel in openDAW, the scope with its
settings beside it; there is no window. Prism Bridge (a socket to the desktop app) is left out.

Upstream's plugins are JUCE around two things this port keeps: Prism's native DSP (`native/src`, plain C++) and its
React plugin UI (`src/plugin-ui`, the desktop app's visualizers in a webview). The JUCE shell is replaced:

```
DAW track audio
  -> process: copy into a ring of the recent audio                                  [plugin/prism.cpp]
  -> per open page, a slot with its own engine, fed block by block (or at the poll)  [plugin/engines.h]
  -> the page polls on each display frame, the engine's frame goes back as binary
  -> page/backend.ts turns it into the event juceBridge.ts listens to (window.__JUCE__.backend)
  -> Prism's visualizers draw it, unchanged
```

## Layout

```
build.sh             builds module.wasm and the page into $OUT
plugin/prism.cpp     the eight CLAP plugins: ring, slots, page messages, state, device panel
plugin/engines.h     the scope engines, after upstream's plugin/Source/*Engine.h, frames as binary
plugin/json.h        a small JSON reader for the settings the page sends (rare)
page/backend.ts      the WebCLAP stand-in for JUCE's webview backend
page/entry.ts        the backend, then upstream's main.tsx
page/index.html      upstream's, its entry ours
page/package.json    upstream's versions of what src/plugin-ui needs
check.mjs            node checks: descriptors, no window, pass-through, every scope's frames, two pages, state, timing (11)
```

`scripts/build-prism.sh` fetches Prism (`scripts/fetch-deps.py`) and applies `patches/prism-webclap.patch`: it lets
`juceBridge.ts` take the frames' arrays as Float32Arrays where JUCE sends base64, adds the device panel's layout to
`ScopeApp.tsx`, and makes the scopes' labels readable (a theme without labels of its own had them in its faint guide
color; now the guides at five times their alpha, and the spectrum draws its frequency labels in that color, not the
grid's). Then it builds, packs and runs the checks.

## What differs from upstream

- **One engine per page.** Upstream's editor owns its engine. Here each page has an id and a slot of its own (up to
  four, a quiet page's freed after five seconds), since a host may show the panel's page more than once and the
  spectrogram's rows and the waterfall's columns come from the canvas.
- **Where the work runs.** Everything runs on openDAW's audio thread. The engines whose DSP runs on the audio's clock
  (spectrogram, meters, vectorscope, waveform, waterfall) take the audio a block at a time, so their work spreads
  over the blocks; the spectrum (its FFT and smoothing run once per push) and the oscilloscope take it at the poll,
  a display frame at a time, as upstream does. A page that does not poll costs nothing beyond the ring.
- **Settings.** Prism's plugins have no parameters; their settings are the UI's JSON, kept by the plugin and saved
  in its state (version, length, the JSON). A change in one page goes to the others. Upstream's app defaults (the
  desktop app's theme and profile read from disk) do not exist here: Prism's default theme.
- **The device panel is the only view** (no `clap.gui`; `page/webclap.css`, a branch of `ScopeApp.tsx` in the
  patch): the scope at a fixed width with its settings beside it, in columns that fit the 176 px without scrolling
  (the wheel is openDAW's), as many rows per scope as leave no control alone in a column. Upstream's gear and
  settings panel below the scope are not used. The spectrum's Reference tab is left out. 316 to 830 px wide.
- **Space is openDAW's** (playback): the controls give the focus back once used, and space never clicks or opens one
  (`page/backend.ts`).
- **Reference tracks** (Spectrum settings > Reference) are not available yet: upstream decodes the file in the
  plugin on a thread. The page answers that they are not available.

## Cost

`check.mjs` prints it (node, one page each, default settings, per 128-sample block at 48 kHz): no page 0.3 µs;
spectrum about 100 µs per poll at FFT 2048, oscilloscope about 300 µs per poll, vectorscope, VU and waveform a few µs
per block, waterfall about 15 µs per block, loudness about 130 µs per block (K-weighting and true peak per sample),
spectrogram about 350 µs per block (its "sharper" frequency reassignment at FFT 4096). `-msimd128` made no difference.
