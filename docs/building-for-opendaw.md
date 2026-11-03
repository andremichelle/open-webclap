# Building a WebCLAP for openDAW

What a WebCLAP bundle needs to work well in [openDAW Studio](https://opendaw.studio): the build, the page, the
parameters, the state and openDAW's device panel. It holds for audio effects and instruments. Note effects have a
guide of their own with the note specifics: [Porting a note effect](porting-a-midi-effect.md).

The plain CLAP ports in this repository are the references, copy from them:

| Port | Shows |
|---|---|
| [WetChorus](../ports/wet-chorus) | The smallest audio effect: one C++ file, a canvas page, the device panel |
| [Signalsmith Basics](../ports/signalsmith-basics) | Seven effects in one bundle, one page, a device panel card each |
| [Helm](../ports/helm) | An instrument with a large HTML editor, parameters in the units the editor shows |
| [Note Lab](../ports/note-lab) | A note effect, the window and the device panel from one page |

## The bundle

A directory, packed as `<id>.wclap.tar.gz` by `scripts/pack-wclap.py`:

```
<id>.wclap/
    module.wasm     the CLAP plugin(s), compiled for wasm32
    index.html      the page (plus what it loads: scripts, images, fonts)
    LICENSE
```

`module.wasm` exports `clap_entry` and imports WASI only, no threads. One module may hold several plugins in its
factory (Signalsmith Basics holds seven); each gets its own clap id and its own page URI.

```sh
em++ plugin.cpp -std=c++17 -O3 -fno-exceptions -fno-rtti -I<clap>/include \
    -o module.wasm -sSTANDALONE_WASM=1 --no-entry \
    -sEXPORTED_FUNCTIONS=_malloc,_free -Wl,--export=clap_entry \
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=4MB -sSTACK_SIZE=256KB -sALLOW_TABLE_GROWTH=1 \
    -sERROR_ON_UNDEFINED_SYMBOLS=1
```

If the code uses more of libc (files, time, random numbers), link `modules/webclap/wasi_shim.cpp`, which keeps the
imports to WASI. JUCE plugins use the kit instead ([README](../README.md#the-juce-kit)).

## One thread

Everything runs on the audio thread of openDAW's worklet: `process`, the page's messages, state and gui calls. No
locks are needed between them, and no threads may be started. Do not allocate in `process`; reserve in `activate`.
`frames_count` is anything from 1 to 128, and blocks are split where automation changes a parameter.

## The page

1. `clap.gui` with the webview API (`CLAP_WINDOW_API_WEBVIEW`) and `clap.webview/3`. `get_uri` answers the page,
   for example `/index.html` (a bundle with several plugins can answer `/index.html#<name>`).
2. The page and the plugin exchange `ArrayBuffer`s: `window.parent.postMessage(buffer, "*")` from the page,
   `message` events back. The bytes are yours; define message types at the top of both files.
3. Keep the traffic small: poll at most every 33 ms, answer with what changed, send animation data as compact
   records and never the whole state.
4. **Resizing.** Answer `gui_can_resize` and resize hints with `preserve_aspect_ratio` and the page's aspect
   ratio, and clamp in `gui_adjust_size` (for example 50 to 200 %). openDAW then offers its zoom levels. Lay the
   page out at one size and scale it to the window.

## Parameters

1. **openDAW maps a CLAP value linearly and shows it as a number**: its knobs, automation and value entry use the
   raw value from `min_value` to `max_value`, they do not ask `value_to_text`. So give each parameter the units
   your editor shows: dB rather than linear gain, percent rather than 0..1, Hz, ms. A typed number then means
   what the user reads. Curves (a knob that spends half its travel below 100 Hz) belong in the page's knob,
   not in the host value.
2. Still answer `value_to_text` and `text_to_value` in those units, for every other host.
3. Mark parameters `CLAP_PARAM_IS_AUTOMATABLE`, stepped ones `CLAP_PARAM_IS_STEPPED` (and `CLAP_PARAM_IS_ENUM`
   for choices).
4. A change the page makes goes to the host as `CLAP_EVENT_PARAM_VALUE` from `process` or `params.flush`
   (call `clap.host_params->request_flush`), with `CLAP_EVENT_PARAM_GESTURE_BEGIN`/`END` around a drag.
5. **Hover.** Report the parameter under the pointer with `clap.param-hovered/1`, on enter, move and press.
   openDAW then offers its parameter menu (automation, MIDI learn) on right-click and its value entry on
   double-click. So:
   - do not use a double-click in the page for anything else (no reset-on-double-click),
   - do not lock the pointer or call `preventDefault` on a press, or the double-click and closing openDAW's menus
     by a click in the page stop working,
   - ignore ctrl+click in drags, it is macOS's right-click.

## State

1. `clap.state` save and load, starting with a version, so a later build reads an older state.
2. **A fixed layout.** openDAW stores the state as bytes and records an edit as the bytes that changed: with a
   fixed layout an edit changes a few bytes, so undo steps and collaboration updates stay tiny. No counters,
   timestamps or animation in the state.
3. **Undo steps.** openDAW saves 250 ms after `clap.host_state->mark_dirty`, and each save is an undo step. A drag
   that only changes the state (not parameters) marks it dirty at its end, not on every step, or a slow drag
   becomes several undo steps. A change that comes from openDAW itself (a project load) is no edit and marks
   nothing.

## The device panel

openDAW shows an effect's or instrument's controls in the device panel of its track. A plugin can put its own
page there instead of openDAW's generic knobs:

1. Add the feature `opendaw:device-panel` to the descriptor. Hosts that do not know it ignore it.
2. Answer the extension `opendaw.device-panel/1`:

   ```c
   typedef struct opendaw_plugin_device_panel {
       // the page for the panel, like clap.webview's get_uri
       int32_t (*get_uri)(const clap_plugin_t *plugin, char *uri, uint32_t capacity);
       // the panel's width for this height
       bool (*get_width)(const clap_plugin_t *plugin, uint32_t height, uint32_t *width);
   } opendaw_plugin_device_panel_t;
   ```

3. **The panel is 176 pixels high**: the device body's 192 px minus its 0.5em padding. openDAW asks
   `get_width(176)` and sizes the panel to the width you answer. Pick the width your layout needs; the panel sits
   in a horizontal row of devices, so keep it as narrow as the controls allow.
4. `get_uri` may answer the window's page or a compact variant (`/index.html?panel`). Both can be open at once and
   both receive everything the plugin sends, so answer with what changed and both stay in sync.

Designing for 176 px:

- Lay out at 176 px high and 1:1 scale, then the panel is pixel exact. The window can show the same layout zoomed
  (Signalsmith Basics' cards, zoomed: sharp on any screen since it is HTML and SVG, with canvases drawn at the
  zoom times `devicePixelRatio`).
- Fill the panel edge to edge: no outer margin, shadow or rounded corners, openDAW draws the device frame around
  it.
- Text below 8 px is hard to read; keep labels at 8 to 9 px and values in a readout or the host's value entry.
- At 176 px there is room for a header (about 34 px), a display and two rows of knobs about 30 px across. More
  controls than that want a wider panel, not a taller one.

## Testing

1. **The site** runs a bundle in openDAW's engine, as the studio does: `cd site && npm run dev`, then
   `http://localhost:8130/<id>?local` loads the archive the build script left in `build/`, with the device panel
   above the window. Import in the header loads any `.wclap.tar.gz`.
2. **A node check** (`ports/<port>/check.mjs`, copy one): a small CLAP host that runs the module without a browser,
   for timing, state bytes per edit and page traffic.
3. **openDAW Studio**: import the bundle on a track.

The plugin's `printf` and `clap.log` lines appear in the browser console. A trap stops the plugin, and openDAW
shows the device as failed; build with `-g` and without `-O3` for function names in the stack trace.

## Sending us the bundle

Send the `.wclap.tar.gz`, the clap id(s) and the version, and raise the version with every build. We add it to
openDAW's catalogue.
