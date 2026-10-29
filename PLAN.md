# JUCE WebCLAP plugins

Run the editor of an existing JUCE plugin as the UI of a WebCLAP plugin, drawn into a canvas, while the
plugin's audio code runs as an ordinary WebCLAP somewhere else. The UI never runs on the audio thread.

Status: OB-Xf runs as a complete WebCLAP bundle (phases 0 and 1): `module.wasm` plays in an AudioWorklet test
host, its unmodified editor runs as `ui.wasm` in the webview page, parameters, gestures and state round-trip. Six
Sines (phase 4) runs the same way, with streams for its meters and spectrum analyzer, and RipplerX with the kit's
first generic extras (keyboard notes, port frames on the AudioProcessor path), and Odin 2, whose editor edits
non-parameter trees directly. Not yet tried in openDAW (phase 2). See "Prototype findings", "DSP module findings",
"Six Sines findings", "RipplerX findings", "Odin 2 findings", "Surge XT findings" and README.md. Written 2026-10-07,
Six Sines and RipplerX 2026-10-08, Odin 2 2026-10-09, Surge XT 2026-10-10.

## Goal

A JUCE plugin author (or someone porting an open-source JUCE plugin) builds two WebAssembly modules from the
same source tree and ships them in one WebCLAP bundle:

- `module.wasm`, the DSP: the plugin as a CLAP, no UI code at all. The host runs it wherever it runs audio
  (in openDAW: an AudioWorklet).
- `ui.wasm`, the editor: the plugin's JUCE editor plus JUCE's software renderer. The bundle's web page loads it
  and draws it into a `<canvas>`.

The two never share memory. They talk only through the WebCLAP webview message channel, which every WebCLAP
host already relays. To the host the result is a normal webview UI, so no host has to change.

## Non-goals

- No change to the WebCLAP spec and no host-specific extension. Anything that only works in openDAW is out.
- No OpenGL. Editors built on `OpenGLContext` (Vital and friends) are not supported by the software path.
- No threads. Neither module may rely on `std::thread` or wasm threads (shared memory). Hosts like openDAW run
  plugins single-threaded.
- Not a generic "any JUCE plugin compiles unchanged" promise. Editors that reach into the engine need glue,
  see "Remote processor".

## Why separate builds

A WebCLAP is one CLAP plugin instance that lives with the audio. A CLAP GUI call such as `clap.webview`
`receive()` reaches that instance between process calls, on the same thread as the audio in single-threaded
hosts. Running a JUCE editor there (layout, repaints, timers) would steal time from audio and cause dropouts.
So the editor gets its own module and its own thread, the page's, and the DSP instance only answers small
messages.

```
 plugin window (iframe, plugin origin)           host                         audio thread
 ┌──────────────────────────────────┐   postMessage   ┌──────────┐   receive()   ┌──────────────────┐
 │ page.js                          │ ──────────────▶ │  WebCLAP │ ────────────▶ │ module.wasm      │
 │  ├─ Worker: ui.wasm              │                 │   host   │               │ (CLAP, DSP only) │
 │  │   JUCE editor + RemoteProc.   │ ◀────────────── │          │ ◀──────────── │  + UI bridge     │
 │  │   software renderer           │   postMessage   └──────────┘  host.send()  └──────────────────┘
 │  └─ OffscreenCanvas              │
 └──────────────────────────────────┘
```

## Components

### 1. `juce_webclap` (JUCE module, UI side)

- **Message loop.** No `MessageManager` thread exists. The page drives it: every animation frame calls an
  exported `tick(timestamp)` that runs pending async updates, fires due `juce::Timer`s and repaints.
- **Software rendering.** The top-level component paints through `LowLevelGraphicsSoftwareRenderer` into a
  `juce::Image` (ARGB). After each tick the dirty rectangles are converted to RGBA and handed to the page, which
  writes them with `putImageData` (or into an `OffscreenCanvas` in the worker). Device pixel ratio is applied by
  rendering at `scale = devicePixelRatio`.
- **Input.** Pointer, wheel and key events from the canvas arrive as small structs and are injected into the
  component tree through a custom `ComponentPeer`. Focus, mouse cursor changes and text input (for the few text
  fields editors have) are mapped back to the page.
- **Fonts.** Bundled TTFs registered with `Typeface::createSystemTypefaceFor` at start, so text renders exactly
  as on desktop. No system fonts exist.
- **Remote processor.** See below.

### 2. Remote processor (UI side)

JUCE editors hold an `AudioProcessor&`. In the UI build the editor gets a stand-in processor that owns the same
parameter layout (`AudioProcessorValueTreeState` or plain `AudioProcessorParameter`s):

- A parameter changed by the editor (including `beginChangeGesture`/`endChangeGesture`) leaves as a message.
- A parameter message from the DSP side sets the stand-in's value with notification, so attachments and
  listeners update the controls.
- Non-parameter state (`getStateInformation`: patch names, wavetables, user content) arrives as a state message
  and is applied with `setStateInformation` on the stand-in. Editor actions that change such state send it back.
- Editors built purely on parameters need nothing else. Editors that call engine methods directly (Surge, Vital)
  need per-plugin sync glue for that data. Measuring this per plugin decides what a port costs.

### 3. UI bridge (DSP side)

A small piece compiled into `module.wasm`, implementing `clap.webview`:

- `get_uri()` names the page in the bundle, `get_resource()` streams bundle files if the host asks for them.
- `receive()` decodes UI messages: parameter value and gesture become CLAP parameter changes the plugin applies
  in its next process/flush, state messages are applied between process calls.
- Parameter changes coming from automation or the host, and state changes (preset load), are sent back with
  `host.send()` from `on_main_thread` after `host->request_callback()`. A host that never calls back gets them
  from `process()` instead, at most 30 times a second (the module has no threads, so it is the same thread).
- Streams for meters, scopes and waveforms: the DSP writes into a ring buffer during process, the bridge drains
  it at UI rate (when the page asks, never from process).

### 4. Page (`ui/index.html`, `ui/page.js`)

- Starts a Worker, loads `ui.wasm` into it, transfers an `OffscreenCanvas`. Falls back to main-thread rendering
  where `OffscreenCanvas` is missing.
- Relays canvas input to the worker and messages between worker and host (`window.parent.postMessage` out,
  `message` events in), the WebCLAP webview convention.
- Sends the "ready" handshake so the DSP side pushes all current parameter values and the state once.

## Message protocol

Binary frames, little endian: `u8 type`, `u8 flags`, `u16 reserved`, payload.

| type | direction | payload |
|---|---|---|
| `hello` | UI → DSP | protocol version |
| `snapshot` | DSP → UI | all parameter values, then a state blob |
| `param` | both | `u32 clapId`, `f64 value` |
| `gesture` | UI → DSP | `u32 clapId`, `u8 begin/end` |
| `state` | both | opaque blob (JUCE `MemoryBlock`) |
| `stream` | DSP → UI | `u16 streamId`, samples (meters, scope data) |
| `resize` | UI → DSP | width, height (the DSP answers through `clap.gui` size requests) |

Parameter ids are the CLAP ids, so a host that maps automation and modulation to CLAP parameters (openDAW does)
stays consistent with what the editor shows.

## Build

- Emscripten for both modules. `module.wasm` through clap-wrapper's wasm target or a direct CLAP entry,
  exporting `clap_entry` and `malloc` like every WebCLAP. `ui.wasm` as a plain Emscripten module exporting
  `init`, `tick`, `input`, `message`.
- JUCE with the platform layer replaced by this module (no X11/Cocoa/Win32), SIMD through wasm SIMD.
- Bundle layout:

```
plugin.wclap/
  module.wasm          DSP, CLAP entry
  memory.json          optional, recommendedInitialBytes
  ui/index.html        page, named by get_uri()
  ui/page.js
  ui/ui.wasm           editor
  ui/fonts/*.ttf
```

## Phases

0. **Spike.** A JUCE `Component` with a slider and some text, compiled to `ui.wasm`, rendered into a canvas in a
   plain web page, mouse working. Answers: does JUCE's software renderer build and run under Emscripten, frame
   cost at 600x400 and 1200x800, binary size, font handling. Starting point: the work behind Surge PR #8597
   (JUCE WebAssembly app).
1. **Remote processor.** A minimal APVTS plugin (gain, filter cutoff) as both modules, connected through a fake
   host page that plays the host's role (relays messages, runs the DSP module). Parameters round-trip both ways,
   gestures arrive, state survives reopening.
2. **Real host.** The same bundle in openDAW: automation and modulation move the editor's controls, editor drags
   record automation, the window survives close and reopen.
3. **Streams.** Meters and a scope at UI rate without touching process time.
4. **First real plugin.** Six Sines (MIT, CLAP-first, Surge team), if its editor turns out parameter-driven, else
   the next candidate. Measure the glue its editor needs. Done, see "Six Sines findings": its editor is not
   parameter-driven, but its engine/editor queues made it a clean port anyway.
5. **Polish.** DPR changes, resizable editors, keyboard focus, text fields, accessibility basics.

## Risks

- JUCE has no official WebAssembly platform layer. The custom `ComponentPeer` and message loop are the bulk of
  the work and must track JUCE releases.
- Editors that are not parameter-driven need per-plugin glue. That may make some well-known plugins expensive.
- Software rendering of large editors at high DPR can be heavy. Dirty rectangles keep the steady state cheap,
  full redraws (resize, theme change) are the worst case. A drawing command buffer replayed on Canvas2D is the
  fallback if pixels prove too slow, at the cost of reimplementing a large part of the graphics interface.
- Binary size: JUCE plus fonts in `ui.wasm` may reach several MB. Bundles are cached by the host, but the first
  open is slower.
- Licenses: JUCE is AGPLv3 or commercial. Open-source ports under GPL/AGPL are fine, closed-source use needs a
  JUCE license. The kit itself should be MIT so it can be used either way.

## Prototype findings (OB-Xf, 2026-10-07)

OB-Xf's editor and processor, unmodified, run as `ui.wasm` in a worker against a fake host page (no DSP module,
no audio). Measured in Chrome on an Apple Silicon Mac:

- **JUCE builds under Emscripten** with the platform layer in `modules/juce_webclap` plus a 130-line JUCE patch
  (`patches/juce-8-wasm.patch`). The patch covers the Timer (no thread, advanced by the frame callback) and gaps in
  JUCE's own wasm target: no `JUCE_LITTLE_ENDIAN` (broke every binary format, including fxp patches and JUCE's
  binary XML), missing `<emscripten.h>`, no thread-priority table, `PluginHostType`, `PropertiesFile`.
- **Software renderer cost.** Steady state 0.1–0.3 ms per frame, knob drags repaint small rectangles. Full redraw of
  the 1438×720 editor: about 25 ms at 1x, 53 ms at 2x (4.1 Mpx). Only resize, zoom and theme changes pay that.
  The pixel fallback (Canvas2D command buffer) is not needed for OB-Xf.
- **Size.** `ui.wasm` 10.8 MB (3.7 MB gzipped) at -O3, not yet tuned. Factory patches and the bitmap theme add
  8.8 MB of data, the embedded vector theme needs none. Startup 180–700 ms including patch scan.
- **Fonts.** HarfBuzz-only typefaces from memory work; DejaVu Sans is the default for JUCE's own widgets.
- **Remote processor.** The stand-in is OB-Xf's own `ObxfAudioProcessor`, never processing audio. The only glue
  needed so far: drain OB-Xf's parameter FIFO every frame (normally done in `processBlock`). Voice LEDs need
  streams (phase 3). A generic parameter-only stand-in would not work for OB-Xf: the editor reaches into the patch
  browser, MIDI learn, voice matrix and theme utils.
- **Protocol.** Plugins re-apply values they were just given (OB-Xf defers patch application by a timer), so the
  bridge remembers what the DSP side has and drops echoes. Parameter ids use `paramID.hashCode()`, which needs
  checking against clap-juce-extensions before phase 2.
- **Display model.** JUCE's display must be the screen space the window may grow into (`screen.availWidth`), not
  the canvas, or editors hide zoom levels. Popups larger than the canvas are clipped.
- **Worker from the start** works (answers the first open question): ui.wasm runs in a worker, paints into a
  transferred OffscreenCanvas with `putImageData` per dirty rectangle (under 1 ms even for full frames).
- **Hidden pages** get no animation frames; the page falls back to a slow timer so protocol traffic continues.

Not done yet: openDAW (phase 2), streams, IME/text input beyond key events, persistent
`/user` storage (settings and saved patches live in memory), keyboard focus polish, size tuning.

## DSP module findings (OB-Xf, 2026-10-07)

`modules/juce_webclap/juce_webclap_clap.cpp` wraps any `juce::AudioProcessor` as a CLAP plugin (params, state,
audio and note ports, tail, gui with the webview API, webview). OB-Xf's processor compiles unchanged with
`OBXF_HEADLESS`, against a GUI-free JUCE (core, events, data_structures, audio_basics,
audio_processors_headless).

- **Standalone wasm.** `-sSTANDALONE_WASM --no-entry` gives `_initialize`, `memory`, `malloc` and the growable
  `__indirect_function_table`; `clap_entry` is exported with `-Wl,--export=clap_entry`. Emscripten still imports
  `env.emscripten_notify_memory_growth` and seven `env.__syscall_*` file calls, which WebCLAP hosts do not
  provide. `modules/webclap/wasi_shim.cpp` defines them, so the module imports only WASI.
- **Traps found by running it:** Emscripten's standalone `getentropy()` calls `abort()` (it kills
  `std::random_device`, used by OB-Xf's randomizer), replaced with WASI `random_get`. JUCE's POSIX
  `InterProcessLock` retries forever when it cannot create its lock file, patched to succeed on wasm (no other
  process exists). A WASI monotonic clock that stands still during a call (`currentTime` in a worklet) makes
  such loops spin forever too, so hosts should use the wall clock.
- **No message thread.** `callAsync` and `Timer::callAfterDelay` (OB-Xf defers patch application by 50 ms) work
  because the wrapper pumps JUCE's queue and timers in process, flush, receive and on_main_thread.
- **Who changed a parameter** decides where it goes: host events go to the page, page edits go to the host as
  output events (with gestures), plugin-internal changes go to both. The DSP never sends the page a state on
  its own, only in snapshots (hello, host state load): two processors sending each other states reload each
  other's patches.
- **Echoes.** After applying a remote state the editor's processor re-sets values (rounded) and reports a state
  change. The UI bridge stays quiet for 300 ms after a remote state, then takes its own serialisation as what
  the DSP has. Without that, reopening the window sent the state back and the host saw a parameter change.
- **Parameter text** is `getText()` only: OB-Xf includes the unit, in its own scale ("6.56 s" for a ms label).
- **Cost.** `module.wasm` 2.5 MB (0.8 MB gzipped). Loads in about 135 ms in Node, a held note costs under 1 % of
  one core in real time.
- **No factory folder in the DSP.** A fresh instance starts on OB-Xf's init values, not its default patch (the
  editor then shows "Init"). Embedding the default patch would match the desktop plugin.

`test-host` is a generic host for any port: `clap-host-worklet.js` is a single-threaded CLAP
host in an AudioWorklet (WASI shim, host callbacks through generated wasm trampolines, events, state, gui,
webview), the page relays the webview, shows parameters, automates one, saves and loads state, plays notes
(screen, computer keys, Web MIDI) and can ignore `request_callback` to test the fallback.

## Six Sines findings (2026-10-08)

Six Sines is CLAP-first: no `juce::AudioProcessor`, so neither `juce_webclap_clap.cpp` nor `ProcessorBridge`
applies. Its editor edits a main-thread patch (`Synth::patchMain`) and talks to the engine through two POD ring
buffers (`MainToAudioMsg`, `AudioToMainMsg`) plus a stereo audio ring for the analyzer. That boundary is what the
port cuts at (`ports/six-sines`, about 1,450 lines, a third of it upstream's CLAP plugin):

- **DSP module.** Upstream's `SixSinesClap` without the JUCE GUI shim, preset discovery and VST3/AU extensions,
  plus `clap.gui` (webview) and `clap.webview`. The engine is unchanged. The bridge does what the editor does on
  the shared main thread: applies `MainToAudioMsg` to `patchMain` and pushes it into the queue, drains
  `audioToMain` while the page is open (`editorActive`), and sends the patch on hello or after a host/preset load
  (`uiForceRebuild`). No JUCE in the module.
- **UI module.** The editor, unchanged, against a stand-in `Synth` that never processes. Per frame the queue
  crosses as one `toAudio` frame (a factory preset load is one 65 KB frame with every value), engine answers arrive
  as `toMain` and go into the stand-in's queue, where the editor's idle drains them as it would.
- **Non-parameter state.** Name, macro names, wavetable blobs, session state: the editor writes them to
  `patchMain` directly. The UI side fingerprints them per frame and sends a `patchMeta` frame on change, with the
  wavetable bytes only when the tables changed. The DSP side builds wavetables itself (upstream does that in the
  editor's idle, so a DSP-only build has to).
- **Port frames.** The kit protocol now reserves types from 64 for ports and has no JUCE dependency.
- **Streams.** VU meters, voice count and CPU ride on `toMain` (about 60 Hz). The spectrum analyzer subscribes to
  the stand-in's audio ring; the UI forwards that as a `scope` frame and the DSP side streams its ring as `audio`
  frames (raw stereo float, about 380 KB/s while the analyzer is open).
- **Threads.** The analyzer's FFT runs on a `std::thread`; `patches/six-sines-no-threads.patch` runs the same loop
  from a 60 Hz `juce::Timer` instead. Nothing else in the editor or engine needed changes.
- **Empty filesystem.** The engine reads user defaults at construction. Emscripten's standalone `stat` answers
  ENOSYS and ghc::filesystem throws on that, so `modules/webclap/wasi_shim.cpp` now answers ENOENT for lookups and
  EROFS for changes: a DSP module looks like an empty, read-only filesystem.
- **Build.** All dependencies at Six Sines' submodule pins under `external/six-sines/libs`, JUCE shared (newer than
  Six Sines' 8.0.10 pin, compiles unchanged). CMakeRC resources work under Emscripten as they are. sst-jucegui
  needs `juce_gui_extra` (colour editor), now `juce_wasm_gui_extra` in `juce_webclap.cmake`.
- **Size.** `module.wasm` 2.6 MB (1.6 MB gzipped), `ui.wasm` 20.7 MB (5.5 MB gzipped), of which about 11 MB is the
  216 embedded factory patches. UI init about 280 ms.
- **Open.** The analyzer is a second top-level window inside the canvas (a separate OS window on desktop) and opens
  over the editor. Saving patches and themes and loading wavetables go through JUCE's own file browser over the
  in-memory `/user`, so files are lost on reload and the user's own wavetable files cannot be reached yet.

## RipplerX findings (2026-10-08)

RipplerX (tilr, GPL-3.0) is an `AudioProcessor` with an APVTS editor, so it takes OB-Xf's path: the kit's DSP
wrapper and `ProcessorBridge`. What its editor does beside parameters became kit features where they are general:

- **Keyboard notes.** Editors with a `MidiKeyboardComponent` play into their processor's `MidiKeyboardState`.
  `ProcessorBridge::forwardKeyboard` sends those notes as `midi` frames (new kit frame type 8), the DSP wrapper plays
  them at the start of the next `process()`.
- **Port frames on the AudioProcessor path.** `PageExtension` (DSP side, `juce_webclap_clap.h`) receives frames from
  type 64 on and gets an `update` call about 30 times a second while the page is open; the UI side is
  `ProcessorBridge::sendPluginFrame` and `onPluginFrame`. RipplerX uses three: polyphony (a setting kept in a
  settings file, not in state), the output meter, and the factory program.
- **Programs must load as programs.** Picking a factory program changes values in the stand-in, which reach the DSP
  side as single parameter changes. RipplerX resets a resonator's ratio when its model changes, which a native
  program load suppresses (`resetLastModels`). Applied value by value, the reset fired and overrode the program's
  ratio (Bells2: 0.47 became 2.0). Now a `program` frame goes ahead of the values and the DSP side calls
  `setCurrentProgram` itself. Plugins that react to parameter transitions need this kind of care.
- **Editors that read state only in their constructor** (RipplerX's program menu) are created once the DSP side's
  snapshot is in, or after a second without one. The kit's main window now requests its size when it is created,
  so an editor that appears after init still sizes the canvas.
- **JUCE GUI in a DSP module.** `AudioProcessorValueTreeState` lives in `juce_audio_processors`, which depends on the
  GUI modules, so `module.wasm` links the GUI JUCE library. With the editor compiled out
  (`patches/ripplerx-headless.patch`) nothing reaches the window code, the linker drops it and the module still
  imports only WASI.
- **JUCE gaps filled:** `MemoryMappedFile` (none on wasm; read-only mappings are now a copy in memory), FLAC sources
  fetched (the mallet samples are FLAC), `juce_audio_utils`' keyboard components compiled without
  `juce_audio_devices` (no wasm backend). `juce_webclap.cmake` gained `juce_webclap_add_module`.
- **Size.** `module.wasm` 1.1 MB (0.4 MB gzipped), `ui.wasm` 5.7 MB (2.3 MB gzipped). UI init under 50 ms.
- **Open.** Popup menus taller than the canvas are clipped (the preset list). Importing presets and user mallet
  samples goes through JUCE's file browser over the in-memory filesystem. A preset imported from a file reaches the
  DSP side value by value, so the ratio reset above can still hit it.

## Odin 2 findings (2026-10-09)

Odin 2 (TheWaveWarden, GPL-3.0-or-later) is an `AudioProcessor` with an APVTS, so it takes the RipplerX path. Its
editor reaches past parameters more than RipplerX's: most of a patch beside the 258 parameters (oscillator and
filter types, mod matrix, drawn waveforms, FX order, LFO and arpeggiator settings) lives in child trees of the
APVTS state ("osc", "fx", "mod", "lfo", "misc", "draw", "midi_learn"), which the editor writes directly and the
processor follows through tree listeners.

- **Tree properties cross over.** The UI side mirrors what the DSP side has of those trees and sends the
  properties that differ, once per frame (`tree` frames, `ports/odin2/odin2_frames.h`). Setting them on the DSP
  side's tree fires Odin's own listeners. Where the editor calls the processor instead of a listener reacting
  (play mode, FX order, drawn wavetables), the page extension makes the same call; drawn tables are rebuilt at
  most 30 times a second, not per moved point.
- **Patch loads are one frame.** A `readPatch` hook (`patches/odin2-webclap.patch`) sends the patch the editor
  loads; the DSP side runs `readPatch` itself. The bridge then takes the stand-in's values as known
  (`ProcessorBridge::assumeRemoteHasCurrentValues`), and port frames now run as page edits on the DSP side
  (parameter changes go to the host, not back to the page; non-parameter changes mark the host state dirty).
  Before that, a preset load sent 30 values one way and 760 the other.
- **Zoom.** The browser build starts at 100 % (800 x 616; Odin's config file does not persist there). The DSP
  side keeps the editor's zoom for the life of the instance (`zoom` frames), so a page that reconnects, e.g. a host
  moving the window into a popout, comes back at the same zoom. It is not saved with the project.
- **Settling counts frames.** After a patch load, a remote state or creating the editor, components write values
  back. The mirror follows the stand-in for 10 frames instead of 300 ms: the editor's first frame takes 450 ms.
- **No modal loops.** Odin uses `PopupMenu::show` for every dropdown and blocking OK/Cancel boxes in its patch
  browser. `patches/odin2-async-dialogs.patch` makes them asynchronous, which also works on desktop.
- **Bitmaps.** 1,248 PNGs drawn for 200 %, scaled to the zoom on 16 threads and cached as files on desktop.
  `patches/odin2-no-threads.patch` scales each when it is first drawn.
- **Font metrics.** Labels were cut off ("Detun", "Maste"). Aldrich's hhea ascent+descent (0.93 em) and OS/2 win
  metrics (1.2 em) differ; JUCE sizes text by them, Windows by win, CoreText and the kit's HarfBuzz typefaces by
  hhea. Odin's layout is sized for Windows and scales by 0.81 on macOS; the browser build scales by the exact ratio.
  The fonts of the other ports have equal metrics.
- **Kit fixes it brought up.** A hidden `TopLevelWindow` (an `AlertWindow` member) became the canvas owner: the
  main window is now the first ordinary window shown, not created. Wheel events no component used (they bubble
  past the window, `patches/juce-8-wasm.patch`) go to the host page, which may scroll; the rest of such a gesture
  follows (latched in the worker by the events' times, until the wheel rests 300 ms), so knobs passing under the
  pointer do not take it. The test host's plugin window scrolls. JUCE places windows for the screen it sees as
  the display: menus and submenus beyond the canvas are now moved inside (Odin's Zoom submenu was unreachable),
  dialogs are centred in the canvas. `binary_data.py` names resources exactly like juceaide (other characters are
  dropped, not replaced: "Chello (MW,AT)").
- **openDAW.** Its plugin window (`FloatingWindow`) keeps the editor's size and scrolls it when the window is
  clamped to the screen, with scrollbars that always show; the frame relays the page's unused wheel events.
- **DSP module.** Upstream's `setStateInformation` shows a message box for newer patches: that pulled JUCE's
  windowing (and its `env` imports) into `module.wasm`; `ODIN_HEADLESS` leaves it out.
- **Size.** `module.wasm` 13.3 MB, `ui.wasm` 38.8 MB (22.5 MB gzipped), the archive 26.9 MB. 10.8 MB of both
  modules is Odin's 160 built-in wavetables (33 band-limited subtables of 512 floats each, which could be computed
  at load), `ui.wasm` also holds 22 MB of bitmaps and the 4.4 MB factory presets. UI init 190 ms, first frame 450 ms.
- **Open.** MIDI learn (the editor arms it on the stand-in, MIDI arrives at the DSP side), tree changes the DSP
  side makes on its own (none known besides MIDI learn) are not sent to the page. Importing presets, soundbanks and
  tunings goes through the in-memory `/user`. The soundbank column of the preset browser stays empty. Bitmaps are
  drawn at the zoom's resolution, so they are soft at a device pixel ratio of 2.

## Surge XT findings (2026-10-10)

Surge XT (Surge Synth Team, GPL-3.0-or-later) is the largest port. Its editor (about 75k lines) calls into
`SurgeSynthesizer` and writes into `SurgePatch` directly (about 1,850 `synth->`/`storage->` uses: scenes, FX, MSEGs,
formula modulators, step sequencers, tuning, modulation routing), so the UI side will need a full engine as its
stand-in, synced by frames. Stages: (1) the engine as `module.wasm`, (2) the editor with a stand-in engine, parameters
and whole patches crossing over, (3) finer frames for MSEG, formula, step sequencer, routing and tuning edits, then
Lua 5.1 for formulas and wavetable scripts.

Stage 1, the engine:

- `ports/surge-xt/CMakeLists.txt` builds surge-common from upstream's own source list (`src/common/CMakeLists.txt`)
  and the libs it links (airwindows, eurorack, sst-*, zstd, sqlite, pffft, r8brain, fmt, binn), with `LINUX` paths
  and the shared sst shim. JUCE (same pin as Surge's) and simde are the shared ones; LuaJIT, MTS-ESP and
  clap-juce-extensions are not fetched (`HAS_LUA=0`, `SURGE_SKIP_ODDSOUND_MTS`).
- Patches: `surge-no-threads` loads a queued patch in the silent block of `process()` instead of starting a thread;
  `surge-webclap` drops the editor (`SURGE_HEADLESS`) and OSC (a stand-in `OpenSoundControl`). `PatchDB` and
  `WtGenService` start threads only on use (patch browser, Lua wavetable scripts), which the engine alone never does.
- `module.wasm` is 6.8 MB and imports WASI only: sqlite's chmod/fchown/ftruncate/utimensat joined the kit's
  read-only filesystem stubs. It links the GUI JUCE library because the processor's headers use
  `PluginHostType` and `Colour`; the linker keeps what is used.
- The page (`ports/surge-xt/ui/index.html`) is a factory patch browser speaking the kit's protocol directly: a patch
  is a state frame with the `.fxp` minus its 60-byte header, which `setStateInformation` loads like a host state.
- All 641 factory patches load and play without NaNs. Silent: the three Lua formula tutorials, the vocoder patches
  and the Audio In templates (the test host feeds no input). CPU in Node (the test host's worklet code), 3-voice
  chords: median 1.2 % of real time, at most 7.3 %. A patch load runs inside one `process()` call: median 2 ms,
  at most 44 ms (a formula tutorial), longer than a 128-sample block, so a host's audio glitches once per load.
- Bundle: 31 MB unpacked, 8.1 MB as `.wclap.tar.gz`, mostly the factory patches (wavetables are inside them).

Stage 2, the editor:

- `ui.wasm` is Surge's editor (upstream's source list, minus OSC and the CLAP preset discovery) with
  `SurgeSynthProcessor` as its stand-in, a full engine that never plays. It compiled with three fixes (no MTS-ESP:
  `uiThreadChecksTunings`; 32-bit `size_t`: an FNV basis; no `juce_audio_devices`: the device-settings check in the
  look and feel). `ui.wasm` is 20.6 MB. The factory data (60 MB, 1,986 files) is Surge's data folder at `/factory`,
  loaded on demand (below).
- Sync: parameters and macros through the kit's bridge (Surge's adapters call `setValueNotifyingHost`), gestures
  included. Patch loads in the editor's engine raise `patchChanged` (the patch adds that for loads without audio;
  with audio, `loadPatchInBackgroundThread` already does), and the glue sends the whole patch as state. Modulation
  routings go through a `ModulationAPIListener` as `modSet`/`modMute`/`modClear` frames, applied with the same API
  on the DSP side. The zoom crosses as a view setting the DSP side keeps for pages that reconnect, as in Odin 2:
  Surge writes its instance zoom into the patch but never reads it back. The DSP side sends its VU peaks and load
  as a `meters` frame (30 Hz while the page is open); their arrival also keeps the editor's VU meter from saying
  "Audio Output Unavailable". Not yet: MSEG, step sequencer, formula, tuning and other non-parameter edits
  (stage 3), and other displays the DSP side would feed.
- The skin defaults to Surge's factory dark skin (user settings live in memory at `/user`).
- No undo history of Surge's own: the host's history covers the plugin through its state (openDAW records parameter
  and state changes and reloads the state on undo), and both reacted to one Cmd+Z in the plugin window. The undo
  manager keeps no records, the undo and redo buttons are gone, their "History" label in the background image is
  covered. The kit's page no longer passes Cmd/Ctrl+Z and Y to any plugin: undo and redo are the host's (text
  fields inside an editor lose Cmd+Z for that).
- Factory data on demand (the kit's `files` option, `webclap-ui-worker.js`): the bundle has `ui/factory/` and a
  manifest of paths and sizes. The worker mounts every file in Emscripten's filesystem with its size, so listings and
  stats need no download, and loads a file's contents when it is first read, with a synchronous request (workers
  may; Surge reads files synchronously). What the editor reads while it starts (about 450 files, 4.4 MB: the dark
  skin, every skin's `skin.xml`, the FX presets Surge parses for its menus, the init patch and a sine wavetable) is
  listed as `prefetch` patterns and downloads in parallel with `ui.wasm`, eight at a time, with retries for dropped
  connections. The page shows one progress bar for both, then a small "Loading <file>" note for files that take
  longer than 150 ms. Before, `ui.data` copied all 60 MB into the wasm heap each time the editor opened.
  In openDAW the bundle is still imported whole (its archive is 43.7 MB) and served by its service worker; the
  synchronous requests go through that service worker too, which is not tested yet.
- Found while doing this: the editor's engine loads Surge's init patch while it is constructed, which raised
  `patchChanged`; with a slower init, the first frame sent that patch to the DSP side before the DSP side's snapshot
  was in, replacing the host's state. Now the construction load is not counted, and nothing goes out before the
  snapshot.
- Kit fixes found here: popup menus now keep to the canvas (the display's area beyond it is a safe-area inset,
  which only menus read), so a menu taller than a zoomed-out editor scrolls or wraps instead of being cut off. A
  submenu with column breaks of its own (Surge's patch categories) that does not fit beside its parent drops them
  and scrolls (`juce-8-wasm.patch`, PopupMenu): placed over its parent, it left the other categories unreachable.
  The test host's worklet wrote through DataViews taken before `malloc` calls that grew the memory.

## coopiano findings (2026-10-20)

coopiano (Peter Cooper, MIT) is a physically modelled piano in header-only C++17: hammer, strings, soundboard
convolution, sympathetic resonance and a room. Its `instrument::Engine` and `piano_host::Adapter` (44.1 kHz inside,
resampled to 48, 88.2 or 96 kHz) have no threads, files or SDK, so the port is a plain CLAP plugin around them
(`ports/coopiano/coopiano_clap.cpp`), without JUCE: `module.wasm` 0.2 MB, WASI only, the archive 0.1 MB, MIT
throughout. Its editor (`ports/coopiano/ui/index.html`, plain HTML and SVG) shows the three parameters as openDAW
Studio's device knobs (the studio's `Knob`, `Column` and `ParameterLabel` geometry, colours and dragging, the 16px
layout scaled to a 352 x 144 window); text messages over `clap.webview/3`, gestures and values to the host as
output events, the host's changes back from `on_main_thread` (or from `process`, 30 times a second at most).

- The cost is in the strike. Every note starts from the hammer's contact with the strings, a nonlinear solve at 32x
  oversampling until the hammer separates: natively up to 3 ms (soft bass) per key and velocity, in WebAssembly
  over 10 ms, longer than an audio block. Upstream's live host solves it on its MIDI thread, or builds all
  88 x 127 responses up front (10 s natively, 72 MiB). A WebCLAP plugin has only the audio thread.
- `patches/coopiano-webclap.patch` makes the solve resumable (`step_contact`, `ContactJob`: the same integration
  stopped and resumed, bit-identical renders against upstream), lets the strike cache start empty and fill one
  response at a time in slices, and adds the runtime output gain and reverb mix the parameters need.
- The wrapper budgets in mode-steps (one string mode by one oversampled step), converted with the speed it measures
  over 50 ms windows (about 7e5 to 9e5 per ms in WebAssembly on an M1; the worklet's clock may tick in whole
  milliseconds, so each window changes the estimate by a quarter at most). A note whose response is missing plays
  at once with the nearest cached velocity of its key (up to 8 steps away) while its own is solved next; without
  one it waits, a quarter of each block solving for it, and starts a few blocks late, once. The background fill
  takes a sixth of each block: what notes asked for, then all keys at velocities 64, 32, 96, 16, 48, 80, 112 and 8
  (5 s on a fast core, half a minute on an efficiency core), so every key has a neighbour within 8 for every
  velocity; the others are solved when played. A solve's setup (copying the key's strings) gets a block of its
  own. The log says "background strikes ready" when it is done.
- The body costs as much as the voices of quiet playing, all the time: 14 % of real time natively in silence, most
  of it once per 256 samples (soundboard convolution: a 1.5 s tail and four 0.2 s bridge regions, each with a
  direct 256-tap head; the sympathetic bank's 992 modes per sample). openDAW's meter shows the slowest block per
  second, so that block read as 60-90 % for one silent instance. The patch now skips silent sympathetic strings,
  multiplies half the spectrum with vectorizable products, one inverse FFT for both channels, spreads the older
  partitions over the block and vectorizes the direct heads, and lets the soundboard and the sympathetic bank
  sleep once their input and state are exactly zero (bit-identical; strings flush below 1e-12 instead of 1e-30,
  -216 dB, so that comes about 13 s after the last note): silence under 1 %, the same sound to rounding.
  The 1.5 s tail is convolved in two sizes: its first 4096 samples in 256-sample partitions, the rest in
  2048-sample partitions whose latency the offset hides, computed spread over the following block (the tail
  from 1.78 % to 0.80 % of real time, playing about 5 % less; on the 0.2 s regions the large FFTs cost more).
  With the pedal down, released notes keep ringing (126 voices on average in the test): those voices are the cost,
  two thirds of it in the voices' mode oscillators (already vectorized upstream; their outputs now sum as a tree,
  about a tenth less). Left to decide: playing keeps solving the exact velocities played (a few ms each, spread at
  a sixth of a block), which a per-block meter sees; quantizing velocities to steps of 4 would let the background
  fill cover all of them, and a voice limit or a cull relative to the loudest voice would bound the pedal case.
- Chrome on an M-series Mac runs a lightly loaded AudioWorklet on an efficiency core: the same `module.wasm` took
  1.2 ms per block there against 0.16 ms in Node, and Node under `taskpolicy -b` reproduces it. Load moves the
  thread to a fast core, so playing can read lower than idling. Idle in openDAW's engine now: 0.17 ms per block on
  the slow core, 0.04 ms on the fast one (Six Sines idling: 0.06 ms).
- openDAW's DSP meter itself: the worklet has no `performance.now()`, so a worker stamps start and end
  (`HRClock`) and the reading includes that worker's wake-up, longer when the CPU idles at low clocks: an idle
  project can read higher than a busy one. The plugin's clock in openDAW is `Date.now()`, whole milliseconds.
- Parameters: Volume (dB on the engine's calibrated gain, before its soft limiter), Reverb (wet level), Sustain
  (damper pedal, continuous: openDAW sends notes but no CC64). MIDI note and CC64 work too. State: the three values.
- Tested in Node with the test host's worklet: the first note 4 blocks late, cached ones in one (the resampler's
  latency); 60 s of random notes at random velocities, median 11-13 % of real time, 99th percentile about 30 %, no
  block over the 2.7 ms budget (the first version: 35 %, 65 %, a few blocks up to 6 ms); sustain, volume, state.
  Mean in silence 1 % (was 15 %), during the first 10 s of filling about 8 %. In the browser: the test host
  plays, and the site in openDAW's engine shows the output on its meter; with `debug.dspLoadMeasurement` on, its
  DSP meter reads 2-26 % idle and 13-26 % with a few notes a second (the first version: 35-75 % idle).
- Open: upstream's voicing (`default.ini`: hammer, strings, soundboard, ...) needs a new engine and an empty cache
  per change, so it is not exposed. Two instances keep two caches. A worker would solve strikes off the audio
  thread, if hosts offer one.

## ZL Audio findings (2026-10-29)

Eight plugins by ZL Audio (zsliu98): ZL Equalizer 2, Compressor, Limiter, Spectrum Equalizer and Splitter (the
current generation, AGPL-3.0, one code base layout), and the older Inflator, Warm and Loudness Match (GPL-3.0).
ZLEComp is archived and left out; Loudness Makeup was ported and removed again (its modes record and play back gain
automation, which made it hard to control in a browser).

- **Shared glue** (`ports/zl-common`): `zl_common.cmake` builds Highway (static dispatch, its WASM SIMD128 target),
  nlopt (its own CMake, Luksan solvers included as upstream) and KFR (sse41 on wasm SIMD, as KFR's CMake does for
  Emscripten) and splits the sources into processor and editor. `zl_ui_main.h` is the ui.wasm entry every ZL port
  shares; a port adds `zlPortInit` and `zlPortFrame`. `zl_analyzer_stream.h` moves zldsp's `AnalyzerSenderBase`
  FIFOs across: the DSP side drains them in `PageExtension::update`, the UI side pushes into the stand-in's sender.
- **Threads.** The current generation's editors run their analyzers and response curves on `juce::Thread`s that
  wait for `notify()` from `paintOverChildren`. The patches make each loop a `do { } while (false)` under
  `ZL_WEBCLAP` and call it from `paintOverChildren`: one pass per repaint, on the editor's thread.
- **JUCE.** ZL builds against a fork of JUCE develop: `Display::userBounds/logicalBounds` (the kit's JUCE:
  `userArea/totalArea`) and `PopupMenu::Item::tooltipText` (missing, so combo box items have no tooltips) are the
  only drift, patched in the plugins.
- **Analyzer traffic.** Raw float for three stereo sources was 1.2 MB/s (ZL Equalizer 2). Now: only the sources the
  editor shows (a mask in the port's controls frame), a source identical to an earlier one as one byte (the side
  source copies the input while no sidechain is used, post equals pre while the EQ is flat), a channel identical to
  the one before as one byte (mono on both channels), a silent channel as one byte, samples as 16 bits with a scale
  per channel (quantisation about 100 dB under the channel's peak). Measured in Chrome at 48 kHz, white noise on
  both channels: 97 kB/s for ZL Equalizer 2, Limiter and Splitter, 193 kB/s for Compressor and Spectrum Equalizer
  (two streams differ), 0.6 kB/s in silence; stereo material doubles the noise figures. The older four send meters
  only: about 1 kB/s. Meters cross as deltas.
- **DSP cost** (Node, the test host's worklet code, 48 kHz, 128-frame blocks): ZL Equalizer 2 with 8 boosted bands
  0.33 % of real time, 8 dynamic bands 1.2 %, linear phase 0.44 %. Nothing worth changing.
- **Kit additions:** every enabled input bus is a CLAP input port, so a sidechain is the second port, which openDAW
  feeds as an aux input (unfed ports are silent); output buses beyond the main one are switched off.
  `clap.latency` (linear-phase modes, lookahead, oversampling; a change while active asks the host for a restart),
  `PageExtension::closed()` (stop what only the page reads: analyzers fill otherwise), and the play head reports
  time in seconds and samples from the seconds timeline or from beats at the current tempo (openDAW sends only
  beats).
- **Splitter:** two stereo outputs upstream; the bundle has one, carrying the first part, or the second with Swap.
- **Trimmed editors:** no presets (the preset browser, ZL Equalizer 2's match target files, ZL Splitter's control
  import/export), no colour schemes (the colour tab; the older four's light/dark switch on the logo), no tooltip
  language (English), no resize corner. The credits tab is a one-line legal notice at the bottom of the settings
  panel (copyright, license, no warranty, source links: GPL-3.0 section 5(d) asks a modified version to keep showing
  what the original shows); the third-party credits and references are in each bundle's NOTICES.md.
- **Open:** UI settings the editors save to files live in the page's memory, ZL Equalizer 2's match EQ
  fits its curve in one editor frame (the editor stalls while it does), ZL Warm's MiSans font has no license file
  upstream.

## Open questions

- Worker with `OffscreenCanvas` from the start, or main thread first and the worker later?
- How a host that has no webview (or no `clap.webview`) should behave: the DSP module must work headless, the
  UI is optional by design.
- Whether to offer the protocol to the WebCLAP community as a convention for split UIs, independent of JUCE.

## References

- WebCLAP: https://github.com/free-audio/web-clap, example plugins https://github.com/WebCLAP/examples
- CLAP webview extension `clap.webview/3`, `clap.gui`, `clap.params`, `clap.state`
- openDAW WebCLAP host notes: `openDAW/plans/webclap.md` (worklet instance, webview relay through a host frame on
  its own origin, parameter and state handling)
- Surge WebAssembly exploration: https://github.com/surge-synthesizer/surge/issues/8581 and PR #8597
- Six Sines: https://github.com/baconpaul/six-sines
