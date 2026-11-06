# Oxo Synth2 port

[Oxo Synth2](https://github.com/chab/oxo_synth2) (chab, MIT), a recreation of Daichi Laboratory's Synth1, as a
WebCLAP bundle. Plain CLAP, no JUCE, and **no upstream patches**: the engine (`EngineCore/`, header-only C++20,
pinned in `scripts/fetch-deps.py`) is already wasm-clean — no threads, no files, no exceptions or RTTI on the
render path — so `oxo_synth2_clap.cpp` is only the wrapper:

- `module.wasm`: the synth as a CLAP instrument (`com.oxo.music.oxosynth2`). 100 parameters: ids 0..98 are the
  patch in Synth1's own index order with their native integer values (what `.sy1` files hold and the panel shows),
  id 99 is the master volume, ids 86..89 (MIDI ctrl src/assign) are hidden. `value_to_text`/`text_to_value` are the
  engine's own `ParamFormat`, so every host shows the LCD's texts. State is a fixed 472-byte layout (an edit
  changes at most 4 bytes) and also loads the macOS plugin's `OXS2` session chunks. Notes snap to the original's
  16-sample sub-block grid, all other events are sample-accurate; the plugin sleeps in silence. WASI-only imports.
- `ui/index.html`: the Synth2 panel redrawn in HTML/SVG on its 770x560 canvas (same sections and widget places as
  `Shared/SynthPanelView.swift`; the SwiftUI editor itself is mac/iOS-only and stays there). Talks to the plugin
  over binary messages (see the top of `oxo_synth2_clap.cpp`), loads the factory bank from `ui/patches.json`
  (built from upstream's first-party `Synth2 Factory` by `patches.py`).

Build: `scripts/build-oxo-synth2.sh` (Emscripten, fetches the pinned engine, runs `check.mjs`, packs
`build/oxo-synth2/oxo-synth2.wclap.tar.gz`). Test in the test host:
`python3 scripts/serve.py .` then `http://127.0.0.1:8123/test-host/?bundle=/build/oxo-synth2/web/oxo-synth2.wclap`.

Known gaps, in priority order:

- No `opendaw:device-panel` page of its own yet (Helm's precedent: openDAW shows its generic parameter panel,
  all 100 parameters with names and texts; the window has the full editor). A compact 176 px panel page would go
  next.
- The patch browser lists the bundled factory bank only; importing `.sy1`/zip banks from disk is not possible
  from a webview page (the host could hand a file over as a message).
- MIDI learn (the mac editor's hotspot learning into `midi_mappings.json`) is not ported; the two ctrl
  src/assign routings are plain parameters.
- Themes and the logo menu (UI size) are not in the web page; the window size comes from the host.
