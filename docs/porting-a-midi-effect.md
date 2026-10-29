# Porting a note effect to WebCLAP

A note effect (a MIDI effect) takes notes and sends notes: an arpeggiator, a harmonizer, a chord generator. In
CLAP it is a plugin with the feature `note-effect`, one input and one output note port and no audio. This guide
takes such a plugin into the browser as a WebCLAP bundle that runs in openDAW's device panel and in every other
WebCLAP host.

[Note Lab](../ports/note-lab) is the reference: a small plain CLAP plugin in C++ that does everything a note
effect in openDAW needs. Read its `plugin/notelab.cpp` and `page/index.html` next to this guide, copy from them
freely (MIT).

Your source stays in your own repository. You build the bundle there, test it with the tools in this repository
and send us the `.wclap.tar.gz`.

## What you need

- Emscripten (emsdk, tested with 3.1.51), Python 3, Node 22 or later, and CMake if your build uses it
- This repository: `git clone https://github.com/andremichelle/open-webclap`
- Chrome or Firefox for testing (Web MIDI works in both)

## Quick start with Note Lab

```sh
scripts/build-note-lab.sh   # fetches the CLAP headers, builds build/note-lab/, runs ports/note-lab/check.mjs
```

The script ends with `note-lab: all 20 checks passed`. Then try it in two places:

1. **The test host**, a plain page without openDAW, quick to debug in:

   ```sh
   python3 scripts/serve.py .
   ```

   Open http://127.0.0.1:8123/test-host/?bundle=/build/note-lab/web/note-lab.wclap and click Start audio.
   Play the on screen keyboard, the computer keys (A W S E D F T G Y H U J K, Z and X for the octave) or a MIDI
   keyboard. The plugin's notes play on a sine, or go to a MIDI output picked under Notes out. The log shows
   every note in and out with its sample. Window and Device panel in the title bar switch between the two
   pages.

2. **The site**, which runs the bundle in openDAW's engine, exactly as the studio does:

   ```sh
   cd site && npm install && npm run dev
   ```

   Open http://localhost:8130/note-lab?local. The page shows the device panel above the plugin window, a
   keyboard (on screen, computer keys, Web MIDI) and an Output select (a sine or a MIDI output device).

## Your bundle

A bundle is a directory, packed as `<id>.wclap.tar.gz`:

```
<id>.wclap/
    module.wasm     the plugin, a CLAP plugin compiled for wasm32
    index.html      the page (plus anything it loads: scripts, images, fonts)
    LICENSE
```

Build `module.wasm` like Note Lab (`ports/note-lab/build.sh`):

```sh
em++ plugin.cpp -std=c++17 -O3 -fno-exceptions -fno-rtti -I<clap>/include \
    -o module.wasm -sSTANDALONE_WASM=1 --no-entry \
    -sEXPORTED_FUNCTIONS=_malloc,_free -Wl,--export=clap_entry \
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=4MB -sSTACK_SIZE=256KB -sALLOW_TABLE_GROWTH=1 \
    -sERROR_ON_UNDEFINED_SYMBOLS=1
python3 scripts/pack-wclap.py <id>.wclap <id>.wclap.tar.gz
```

The module may import WASI only, and there are no threads. If your code uses more of libc than Note Lab (files,
time, random numbers), link `modules/webclap/wasi_shim.cpp`: an empty read-only file system, the memory growth
hook and entropy, so the module still imports WASI only. Check with
`node -e "console.log(WebAssembly.Module.imports(new WebAssembly.Module(require('fs').readFileSync('module.wasm'))))"`.

## The contract

### Descriptor and ports

1. Features: `note-effect` and, for the device panel, `opendaw:device-panel`. A host that does not know the
   second one ignores it.
2. `clap.note-ports`: one input and one output port, both with `CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI`,
   preferred CLAP.
3. `clap.audio-ports`: implement it and answer 0 ports in both directions. Without it some hosts assume stereo.

### Processing

1. Notes arrive at their sample in `process`: `header->time` is the offset in the current call. Send your notes
   the same way, with `try_push` on the output events, sorted by time. openDAW keeps their sample: a note sent
   at offset 37 reaches the instrument at sample 37.
2. `frames_count` is anything from 1 to 128. A block is split where automation changes a parameter, so count
   time in samples you were given, never in calls.
3. The transport event carries tempo and song position in beats. Use it for anything tempo synced.
4. Every note on you send gets a note off from you. When a parameter change makes held notes meaningless (a mode
   change), send their note offs at that sample. On `reset`, `stop_processing` and `deactivate`, only forget
   them: the host ends what it got from you.
5. A key you hold already: send a note off before the next note on of that key, so the stream never holds a key
   twice. Note Lab counts its sources per key and re-attacks.
6. Input `CLAP_EVENT_MIDI` with notes (0x90, 0x80) as well as CLAP notes. Pass other MIDI messages (sustain,
   pitch bend) through unchanged.
7. No allocation in `process`. Reserve what you need in `activate`.
8. Everything runs on one thread, the audio thread in the worklet: `process`, the page's messages, state, gui
   calls. Do not start threads.

### Parameters

1. Mark them `CLAP_PARAM_IS_AUTOMATABLE`, stepped ones `CLAP_PARAM_IS_STEPPED` (and `CLAP_PARAM_IS_ENUM` for
   choices), and answer `value_to_text`. openDAW shows them in the device's Parameters menu, automates them and
   maps them with MIDI learn.
2. A value that changes because of the plugin itself (a preset loads, the page moves a control) goes out as
   `CLAP_EVENT_PARAM_VALUE`, from `process` or `params.flush`. The page's controls send gesture begin and end
   around a drag.
3. **Presets, a suggestion.** openDAW passes notes to a note effect, not program change or CC messages. A
   stepped Preset parameter lets users recall presets by automation and MIDI learn, from any controller. Note
   Lab does that with its eight presets A to H. Your call.

### State

1. `clap.state` save and load, with a version, so a later build reads an older state.
2. Call `clap.host_state.mark_dirty` when the page changes something.
3. **Keep the layout fixed** so an edit changes few bytes. openDAW stores the state as bytes and records an edit as
   the bytes that changed: a fixed layout makes every history step and every collaboration update tiny. Note
   Lab's state is 72 bytes, a parameter edit changes at most four of them. Never put a counter, a timestamp or
   the animation in the state.

### The page

1. `clap.gui` with the webview API (`CLAP_WINDOW_API_WEBVIEW`) and `clap.webview/3`: `get_uri` answers
   `/index.html`, the page every host shows in its window.
2. The page talks to the plugin with `window.parent.postMessage(arrayBuffer, "*")` and receives `message` events
   with an `ArrayBuffer`. The bytes are yours, define your own message types (Note Lab's are at the top of both
   files).
3. **Device panel.** `opendaw.device-panel/1` (the struct is in `notelab.cpp`): `get_uri` answers
   `/index.html?panel`, the same page in its compact layout, and `get_width` the width for the panel height of
   176 pixels (Note Lab: 352). The panel shows the preview, the presets and a Big View button.
4. **Big View** asks the host for the window: the page sends a message, the plugin calls
   `clap.host_gui->request_show`.
5. Both pages can be open at once. The plugin sees one page, both receive everything it sends, so answer with
   what changed and both stay in sync.
6. Report the parameter under the pointer with `clap.param-hovered/1`. openDAW then offers its parameter menu
   (automation, MIDI learn) on right click and value entry on double click. Ignore ctrl+click in your own drags,
   it is macOS's right click.

### Data exchange

Messages between page and plugin cross the worklet, keep them few and small:

1. The page says hello when it loads and gets one snapshot of everything.
2. After that it asks at most every 33 ms, and the plugin answers only with what changed since the last
   answer, nothing at all if nothing did.
3. Animation data (the notes, levels) goes as compact records, never as the whole state.

Note Lab's numbers to compare with: an 80 byte snapshot, 4 bytes per note event, 8 bytes per changed value,
no message while nothing happens.

## Checking your build

1. `ports/note-lab/check.mjs` is a small CLAP host in node. Copy it next to your plugin and change the tests to
   your plugin's behaviour: note timing, held notes across calls, the state bytes per edit, the bytes per page
   message. `node check.mjs <id>.wclap/module.wasm` runs in a second, and needs no browser.
2. In the test host: notes in and out with their samples, both pages, Big View, Save state and Load state.
3. On the site: Import in the header loads your `.wclap.tar.gz` and shows it like Note Lab, in openDAW's engine.
4. In openDAW Studio: add the WebCLAP MIDI effect to an instrument track and import your bundle there.

## Debugging

- The plugin's `printf` and `clap.log` lines appear in the browser console (the worklet's console).
- A trap (a crash in the plugin) stops the plugin: the device shows it as failed and passes the notes through.
  Build with `-g` and without `-O3` to get function names in the stack trace.
- In Chrome, the worklet's console is in the page's console. Breakpoints in the wasm need the debug build and
  the DWARF extension for Chrome DevTools.

## If your plugin uses JUCE

The JUCE kit in this repository (`modules/juce_webclap`) does not send MIDI out of a plugin yet. Tell us before
you start, we add it to the kit first (`producesMidi`, `isMidiEffect` as `note-effect`, an output note port).

## Sending us the bundle

Send the `.wclap.tar.gz`, the clap id and the version. Raise the version with every build you send. We add it to
openDAW's catalogue. The bundle stays yours: it is
not added to this repository, and its license is the one you put in its LICENSE file.
