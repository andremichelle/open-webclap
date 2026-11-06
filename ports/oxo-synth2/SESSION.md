---
title: "Porting Oxo Synth2 to WebCLAP in open-webclap"
subtitle: "Annotated session transcript, with reasoning"
author: "Mistral Vibe — session log for Björn"
date: "6 November 2026"
lang: en
---

# About this document

This is a reconstruction of a single working session on the `open-webclap`
repository (the fork `synth-is/open-webclap`, branch `main`). It records what
was asked, what I reasoned, what I did, and — importantly — where I was wrong
and how I found out.

The session produced seven commits on `main`, ready for a pull request to
`andremichelle/open-webclap`:

| Commit | Subject |
|---|---|
| `e40f09b` | Oxo Synth2 port: CLAP wrapper (module.wasm) around the pinned engine |
| `651e88b` | Oxo Synth2 port: editor page, build script, site entry, README row |
| `956bdcd` | Oxo Synth2 page: fix addDisplay return and guard setPointerCapture |
| `7189fb4` | Oxo Synth2 port: version follows the pinned upstream commit; document updates |
| `1dba42a` | Oxo Synth2 page: widgets must be children of their section |
| `9827adc` | Oxo Synth2 port: ship both upstream patch banks; fix bottom-bar and label layout |
| `5d26ef2` | Oxo Synth2 page: center labels/LED radios/wave grids like SwiftUI's position() |

---

# Part 1 — The request

> **User:** …I would like to contribute to and utilise the porting and testing
> support of that project and try porting oxo_synth2 — upstream repo at
> `github.com/chab/oxo_synth2`. … Please study both projects and start porting
> oxo_synth2 to the WebCLAP format: I'm not sure if there should be a new repo
> for that; you can decide and then create it. There are agent-specific
> instructions for the Oxo Synth2 in `oxo_synth2/AGENTS.md`. Spawn sub-agents
> as required.

## Reasoning

Two decisions hide in the first paragraph. The repo question: open-webclap's
own README says every port is a folder `ports/<port>/` with the upstream
source pinned in `scripts/fetch-deps.py`, so a separate repo would duplicate
the kit, the test host, the pack/license scripts and the site for no benefit.
Decision: **no new repo**; the port lives in the fork as `ports/oxo-synth2/`.

The second decision is which port pattern to copy. open-webclap has JUCE
ports (wrapped by `modules/juce_webclap`) and plain-CLAP ports. oxo_synth2 is
not JUCE: its engine (`EngineCore/`, header-only C++20) is portable and its
editor is SwiftUI (mac/iOS-only), so the Helm/coopiano pattern fits — engine
behind a small C++ CLAP wrapper, editor replaced by an HTML page. The SwiftUI
panel can't be reused (and must not be: clean-room rules forbid copying
artwork), but `Shared/SynthPanelView.swift` is the authoritative layout spec
to redraw.

`oxo_synth2/AGENTS.md` constrains the upstream side: clean-room (no
disassembly, no artwork), no third-party preset files, `Shared/` stays
iOS-clean, submodules never edited, commit at every significant step. Since
the port never modifies the oxo_synth2 repo — it pins a commit and patches go
to `patches/` if ever needed — those rules were respected by construction.

## What I did

Studied both repos with two sub-agents (one per repo) plus my own reading:
open-webclap's `docs/building-for-opendaw.md` checklist, the Helm wrapper
(`ports/helm/helm_clap.cpp`) as the instrument pattern, pearls/wet-chorus as
the small-port pattern, `fetch-deps.py`/`pack-wclap.py`/`bundle-license.sh`,
the test host and `site/src/plugins.ts`; and oxo_synth2's `EngineCore` API
(`Synth.hpp`, `Params.hpp`, `Sy1Codec.hpp`, `ParamFormat.hpp`), the VST3
processor's event loop, and `SynthPanelView.swift`'s exact panel layout.

A portability audit showed the engine is already wasm-clean: no threads, no
filesystem, no exceptions/RTTI on the render path (verified by compiling with
`clang++ -std=gnu++20 -fno-exceptions -fno-rtti`), no allocation in `Render`
after `SetSampleRate`, deterministic seeded RNG, ~37 KB struct + ~3 MB of
wave/filter tables built once per sample rate. The only flag: `M_PI` under
musl → build with `gnu++20`. No upstream patches were needed at all.

---

# Part 2 — The DSP side: `oxo_synth2_clap.cpp` + build

## Reasoning

The wrapper mirrors what the VST3 processor does, because that is the
proven host integration: `BeginBlock(frames)` per host block, `SetTransport`
from the transport, events merged in time order with note on/off snapped to
the original's 16-sample sub-block grid (`NoteEventOffset`) and everything
else (parameters, CC, bend) sample-accurate, rendering split at each event.

Parameters are the 99 patch values as CLAP ids 0..98 in Synth1's index order
with their **native integer ranges** — the value a host maps linearly between
min and max is exactly what `.sy1` files hold and the panel shows — plus id
99 for the master volume, ids 86..89 hidden (MIDI ctrl routing, as in the
original). `value_to_text`/`text_to_value` use the engine's own `ParamFormat`,
so every host shows the LCD's texts ("LP24", "1/8", "+0.0 dB", …).

State is a fixed 472-byte layout (`'OS2W'` + version + 99×i32 + volume +
64-byte name), so an edit changes at most 4 bytes; loading also accepts the
macOS plugin's own `'OXS2'` chunks (v1..v4, `.sy1` text tail) so sessions move
between formats.

## What I did

- `scripts/fetch-deps.py`: pinned `chab/oxo_synth2` at `5bce0fd` (the local
  clone's HEAD), fetching `EngineCore/`, `Identity.xcconfig`, `LICENSE`,
  `README.md`, and the shipped patch banks; added the `oxo-synth2-` prefix to
  `PATCH_TARGETS` (no patches exist).
- `ports/oxo-synth2/CMakeLists.txt`: emcmake, gnu++20, `-msimd128 -O3
  -fno-exceptions -fno-rtti`, the kit's `wasi_shim.cpp`, standalone-wasm link
  flags like helm's → 175 KB `module.wasm` importing **WASI only**
  (`wasi_snapshot_preview1.proc_exit`). Descriptor version is parsed from the
  pinned `Identity.xcconfig` (`MARKETING_VERSION`), so it tracks the pin.
- `oxo_synth2_clap.cpp`: the wrapper (descriptor `com.oxo.music.oxosynth2`,
  audio-ports stereo out, note-ports CLAP|MIDI, params, state, tail=∞, gui
  webview 770×560 @ 0.5–2×, webview `receive`, sleep-in-silence with
  `CLAP_PROCESS_SLEEP`, page protocol hello/poll/begin/set/end/hover/
  patch/init/panic ↔ describe/snapshot/values/activity).

## Verification, and a real bug it exposed

`check.mjs` (node CLAP host modeled on helm's) covers: WASI-only imports,
descriptor/features, 100 params with ranges/flags/texts both ways, hello →
describe (65 KB JSON) + 468-byte snapshot, idle polls send nothing, a page
drag = one gesture (begin/value/end) + one dirty mark + the value applied,
host param events answered to the page on the next poll, state 472 bytes with
≤4 bytes changed per edit and byte-identical round trip, `'OXS2'` import,
note at sample 77 sounds from sample 65 (16-sample grid), short note, reset,
sleep after 0.5 s and wake on a note, MIDI note-on/sustain/all-notes-off, page
patch = one gesture + snapshot carries the name, INIT patch, and DSP timing.

Two bugs were found by testing rather than reading:

1. My first harness wrote MIDI `data` at struct offset 16; `clap_event_midi`
   has `port_index` there and `data` at 18. The wrapper was right, the harness
   wrong — and the mismatch showed up as "the wasm build ignores CC" until a
   native-vs-wasm render comparison isolated it to exactly sample 30000.
2. A sustain test "failed" because the earlier legacy-chunk load had left
   every parameter at 64 — the delay effect on, ringing for 2 s. The engine
   was right; the test's patch was bad.

The strongest check: the same notes/params/bend/CC job rendered through
`module.wasm` in node and through the native engine differs by max 1.5e-8
(−162 dB rms) — libm rounding only. The wasm build sounds identical to native.

DSP cost: 96 µs per 128-frame block at 48 kHz with an 8-note × 8-unison chord
= **3.6 % of real time**.

---

# Part 3 — The editor page: `ui/index.html`

## Reasoning

The page redraws the Synth2 panel on its 770×560 design canvas (same sections,
same widget kinds, same coordinates from `SynthPanelView.swift`), scaled to
the window by a CSS transform. All widgets are drawn (SVG knobs, CSS/SVG wave
icons, LED radios, cyan LCDs) — no upstream artwork. Protocol: binary LE
messages over `postMessage`, page says hello → gets the JSON parameter table
(prebuilt in `init`, so nothing allocates on the audio thread) and a
snapshot, then polls at most every 33 ms and gets only what changed. A drag
is one gesture (one undo step); hover is reported so the host's parameter
menu/value entry work; no double-click/right-click/wheel handlers of its own,
no `preventDefault` on presses, no pointer lock — the host owns those.

## What I did and found

- Page + `patches.py` (factory bank → `patches.json`) + `NOTICES.md.in` +
  `scripts/build-oxo-synth2.sh` + `site/src/plugins.ts` + README table row.
- A jsdom harness ran the real page against a fake plugin: it caught
  `addDisplay` returning the widget instead of the element, and an
  unguarded `setPointerCapture`.
- Headless Chromium (Playwright, installed in the scratchpad; the Playwright
  MCP server was also added to the Vibe config with `vibe mcp add playwright
  …`) drove the actual test host: knob drag moves the host's parameter row,
  host slider moves the page knob, patch buttons move every control, zero
  console errors.

The layout bugs the user then reported were real and were all mine: widgets
appended to the stage with section-relative coordinates (everything piled
top-left); then SwiftUI's `.position()` being a **center**, not a top-left
point, for labels/LED lists/wave grids ("random" and LP12…LPDL overflowing,
labels under Wheel/MIDI buttons); then the effect-knob labels — upstream uses
static `ctl1/ctl2/level` under the knobs (type-specific names go to the LCD).
After those fixes the automated check reports no overflow, no overlaps.

---

# Part 4 — Keeping up with upstream; presets

## Reasoning

The pin in `fetch-deps.py` is the single update point. Bumping it refetches
the dep, reapplies any `patches/oxo-synth2-*.patch`, and — because the filter
also pulls `Identity.xcconfig` and both shipped bank folders — carries the
descriptor version and any new patches automatically. This workflow is
documented in `ports/oxo-synth2/README.md` ("Keeping up with oxo_synth2").

On presets: upstream's own installers ship two banks (`Synth2 Factory`, 51
patches, and `Synth2 Starter Banks/Madame Eljay`, 128), so the bundle ships
the same 179 as `patches.json`, grouped by bank in the page's browser. The
`.sy1` version travels with each patch so the plugin runs the engine's legacy
conversion for pre-112 versions. Third-party banks from the web are *not*
bundled — oxo_synth2's AGENTS.md forbids preset files whose designers didn't
license them.

---

# Appendix A — Known gaps (deliberate, listed in the port README)

- No `opendaw:device-panel` page of its own yet — openDAW shows its generic
  parameter panel (all 100 parameters, named, with texts), as it does for
  Helm; the window has the full editor. A compact 176 px panel page is the
  natural next step.
- No `.sy1`/zip import from disk (a webview page can't read files; a host
  could hand a file over as a message), no MIDI learn, no themes/logo menu.

# Appendix B — What was not established

- No browser test inside openDAW Studio itself (no full DAW here); the test
  host and the site page (`/oxo-synth2?local`) were exercised headlessly and
  by the user, audio included.
- `scripts/fetch-deps.py` fails on *other* ports' deps in this environment
  because the shared IP's unauthenticated GitHub API limit was exhausted; it
  self-heals on reset (or with a token). This port's deps were already
  fetched and stamped.
- No golden-audio assertion in `check.mjs` on purpose: when the upstream pin
  is bumped, engine sound changes are upstream's business, gated by their own
  `make -C EngineCoreTests test` against the Synth1 reference renders.

# Appendix C — Files produced

| Path | Lines | Role |
|---|---|---|
| `ports/oxo-synth2/oxo_synth2_clap.cpp` | 1156 | CLAP wrapper around the pinned engine |
| `ports/oxo-synth2/CMakeLists.txt` | 56 | emcmake build → `module.wasm` (WASI-only) |
| `ports/oxo-synth2/ui/index.html` | 786 | Synth2 panel redrawn in HTML/SVG + protocol |
| `ports/oxo-synth2/patches.py` | 78 | upstream banks → `ui/patches.json` (179 patches) |
| `ports/oxo-synth2/check.mjs` | 574 | node CLAP host test suite |
| `ports/oxo-synth2/NOTICES.md.in`, `README.md`, `SESSION.md` | — | licenses/notices, port doc, this log |
| `scripts/build-oxo-synth2.sh` | 38 | fetch → build → check → license → pack |
| `scripts/fetch-deps.py` | +14 | `oxo-synth2` pin + `PATCH_TARGETS` entry |
| `site/src/plugins.ts`, `README.md` | +17 | `/oxo-synth2` page, table row, build command |

# Appendix D — A note on method

Most of the substantive bugs in this session were found by executing, not by
reading: the MIDI offset bug only appeared when native and wasm renders were
compared sample by sample; the sustain "failure" was a bad test patch, not a
stuck note; every page layout bug was confirmed from a rendered screenshot
and an element-overlap query, not from eyeballing coordinates. The one class
of bug that *did* come from reading was the SwiftUI `.position()` center
semantics — which is why the layout was verified with measurements
(bounding boxes) instead of screenshots alone.
