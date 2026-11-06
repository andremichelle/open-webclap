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
  over binary messages (see the top of `oxo_synth2_clap.cpp`), loads the patch banks from `ui/patches.json`
  (built by `patches.py` from the banks upstream's own installers ship: `Synth2 Factory` + `Synth2 Starter
  Banks`; third-party `.sy1` banks from the web are not bundled — their designers license them nowhere).

Build: `scripts/build-oxo-synth2.sh` (Emscripten, fetches the pinned engine, runs `check.mjs`, packs
`build/oxo-synth2/oxo-synth2.wclap.tar.gz`). Test in the test host:
`python3 scripts/serve.py .` then `http://127.0.0.1:8123/test-host/?bundle=/build/oxo-synth2/web/oxo-synth2.wclap`.

## Keeping up with oxo_synth2

The port tracks upstream by its pinned commit, one line in `scripts/fetch-deps.py`
(`("oxo-synth2", "chab/oxo_synth2", "<commit>", …)`). To take an upstream improvement:

1. Bump the commit in that `DEPS` entry and update the commit named in `NOTICES.md.in`. Run
   `python3 scripts/fetch-deps.py` — the changed dep is refetched (the `.fetched` stamp differs) and any
   `patches/oxo-synth2-*.patch` are applied (`PATCH_TARGETS` already has the prefix; there are none today).
   If a patch no longer applies, rebase it. The fetch filter pulls `EngineCore/`, `Identity.xcconfig`,
   `LICENSE`, `README.md` and `Shared/Resources/Synth2 Factory/` — new factory patches are picked up by
   `patches.py` automatically, and the descriptor's version is read from the pinned `Identity.xcconfig`
   `MARKETING_VERSION` in `CMakeLists.txt`, so no other version string needs editing.
2. Clean build and test: `rm -rf build/oxo-synth2 && scripts/build-oxo-synth2.sh` (runs `check.mjs`: the
   parameter contract, state layout, note/MIDI behavior, page protocol, DSP timing). If the engine's API or
   the panel changed, fix the wrapper (`oxo_synth2_clap.cpp`) and the page layout (`ui/index.html` mirrors
   `Shared/SynthPanelView.swift`) in this repo.
3. If upstream itself needs a change to build/run as wasm (a new non-portable include, …), put it in a small
   `patches/oxo-synth2-<name>.patch` with a header comment describing it, per the repo's convention — prefer
   hooks over rewrites. Ideally propose such changes upstream so the pin eventually needs no patch.

Upstream's own correctness gate (`make -C EngineCoreTests test` in oxo_synth2) covers the engine against the
original Synth1's reference renders; this repo's `check.mjs` covers the wrapper contract. When intentionally
bumping the pin, listen for engine sound changes — that is upstream's business, not a port regression.

Known gaps, in priority order:

- No `opendaw:device-panel` page of its own yet (Helm's precedent: openDAW shows its generic parameter panel,
  all 100 parameters with names and texts; the window has the full editor). A compact 176 px panel page would go
  next.
- The patch browser lists the bundled factory bank only; importing `.sy1`/zip banks from disk is not possible
  from a webview page (the host could hand a file over as a message).
- MIDI learn (the mac editor's hotspot learning into `midi_mappings.json`) is not ported; the two ctrl
  src/assign routings are plain parameters.
- Themes and the logo menu (UI size) are not in the web page; the window size comes from the host.
