# Building a WebCLAP for openDAW

What a WebCLAP bundle needs to work well in [openDAW Studio](https://opendaw.studio): the build, the page, how it is
used, how it looks, the parameters, the state, the data between page and plugin, and speed. It holds for audio effects
and instruments. Note effects have a guide of their own with the note specifics:
[Porting a note effect](porting-a-midi-effect.md).

**Target openDAW first.** A bundle runs in any WebCLAP host, but its users meet it in openDAW, and mostly in the
device panel of a track, next to openDAW's own devices. Design for that panel first, the window second, other hosts
last: they get correct parameters, state and texts, openDAW gets the care.

The plain CLAP ports in this repository are the references, copy from them:

| Port | Shows |
|---|---|
| [WetChorus](../ports/wet-chorus) | The smallest audio effect: one C++ file, a canvas page, the device panel |
| [Signalsmith Basics](../ports/signalsmith-basics) | Seven effects in one module, one page, a device panel card each, an update of an existing bundle |
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
imports to WASI. JUCE plugins use the kit instead ([README](../README.md#the-juce-kit)). Check the imports with
`node -e "console.log(WebAssembly.Module.imports(new WebAssembly.Module(require('fs').readFileSync('module.wasm'))))"`.

## One thread

Everything runs on the audio thread of openDAW's worklet: `process`, the page's messages, state and gui calls. No
locks are needed between them, and no threads may be started. Do not allocate in `process`; reserve in `activate`.
`frames_count` is anything from 1 to 128, and blocks are split where automation changes a parameter: count time in
the samples you are given, never in calls, and apply each parameter event at its sample (render up to the event,
apply it, go on).

## The page

1. `clap.gui` with the webview API (`CLAP_WINDOW_API_WEBVIEW`) and `clap.webview/3`. `get_uri` answers the page,
   for example `/index.html` (a bundle with several plugins can answer `/index.html#<name>`).
2. The page and the plugin exchange `ArrayBuffer`s: `window.parent.postMessage(buffer, "*")` from the page,
   `message` events back. The bytes are yours; define message types at the top of both files.
3. **Resizing.** Answer `gui_can_resize` and resize hints with `preserve_aspect_ratio` and the page's aspect
   ratio, and clamp in `gui_adjust_size` (for example 100 to 300 %). openDAW then offers its zoom levels. Lay the
   page out at one size and scale it to the window.
4. **The page is clipped at its edges** (it runs in an iframe). Keep everything that pops up, a value readout, a
   menu, inside it: show a value in place of the control's label rather than above a knob at the top edge.
5. Load what the page needs from the bundle or a CDN that is always there (Google Fonts works). No network calls of
   your own.

## How it is used

openDAW owns some gestures on the page, as soon as the plugin reports the hovered parameter. Leave them to it.

1. **Hover.** Report the parameter under the pointer with `clap.param-hovered/1`, on enter, move and press, and
   keep it while a drag runs. openDAW then offers its parameter menu (automation, MIDI learn) on right-click and its
   value entry on double-click.
2. **No double-click** of your own: no reset-on-double-click, no double-click to open anything. It is openDAW's
   value entry. A single click must not change a value where a double-click is expected either (a choice that steps
   on a click would step twice before the entry opens): let arrows step, not the whole control.
3. **No right-click** of your own, it is openDAW's parameter menu. Ignore ctrl+click in drags, it is macOS's
   right-click.
4. **No value wheel.** The wheel scrolls openDAW's devices and pages; a knob that takes it changes values by
   accident while the user scrolls past. Values change by dragging and by openDAW's value entry.
5. **No pointer lock, no `preventDefault` on a press.** Either stops the double-click from reaching openDAW, and a
   click in the page from closing openDAW's menus. `setPointerCapture` for a drag is fine.
6. **Keys.** Plain keys reach both the page and the studio (the computer keyboard plays notes), shortcuts with cmd
   or ctrl only the studio. Do not build on keys; never on space (the transport).
7. **A drag is one gesture**: `CLAP_EVENT_PARAM_GESTURE_BEGIN`, the values, `END`. openDAW records it as one undo
   step.

## Looking like a stock device

The panel sits in a row with openDAW's own devices. A plugin that looks like one of them feels built in.

- **No surface of your own.** No background, border, shadow or rounded corners around the content: openDAW draws the
  device frame. Give the page's `body` the panel's colour, `--color-panel-background`, `hsl(197, 14%, 12%)` in the
  default (classic) scheme, so the panel is seamless. Previews of the page can put the devices on the studio's
  `--color-background`, `hsl(197, 8%, 9%)`.
- **The studio's type**: Rubik (Google Fonts), labels 8 px, `--color-gray` (`hsl(197, 31%, 91%)`) for text, a unit
  in `--color-dark` (`hsl(197, 15%, 84%)`) after the value, `--color-blue` (`hsl(189, 100%, 65%)`) while a control
  is modified, `--color-cream` (`hsl(65, 37%, 83%)`) for headings (h5 at 0.5em, capitalized).
- **The studio's knob** (`openDAW/packages/app/studio/src/ui/components/Knob.tsx`): a track of 1.5 px around the
  knob, open at the bottom by π/5 on each side, the value as an arc from its anchor (0 or the centre), an indicator
  of 2.5 px; 1.75em across.
- **The studio's grid**: devices lay their controls on columns of 3.5em, three rows of 3.5em, 0.25em apart, in a body
  of 192 px.
- Copy the studio's styles as they are (its `.sass` files in `openDAW/packages/app/studio/src/ui/`), add no touches
  of your own (opacity, hover states, outlines). The studio has colour schemes the page cannot read; the classic
  values are the safe default.

A skin of its own is fine too (Signalsmith Basics has one: cards with LCD displays). Then still keep the rules above
that are not about looks: no surface, the panel's colour, 176 px, the studio's gestures.

## Parameters

1. **openDAW maps a CLAP value linearly and shows it as a number**: its knobs, automation and value entry use the
   raw value from `min_value` to `max_value`, they do not ask `value_to_text`. So give each parameter the units
   your editor shows: dB rather than linear gain, percent rather than 0..1, Hz, ms. A typed number then means
   what the user reads. Curves (a knob that spends half its travel below 100 Hz) belong in the page's knob,
   not in the host value. (An update of an existing bundle keeps that bundle's values instead, see below.)
2. Still answer `value_to_text` and `text_to_value` in those units, for every other host.
3. Mark parameters `CLAP_PARAM_IS_AUTOMATABLE`, stepped ones `CLAP_PARAM_IS_STEPPED` (and `CLAP_PARAM_IS_ENUM`
   for choices).
4. A change the page makes goes to the host as `CLAP_EVENT_PARAM_VALUE` from `process` or `params.flush`
   (call `clap.host_params->request_flush`), with the gesture around a drag.
5. Keep a parameter's exact value where a round trip would lose it: a value that goes through a knob curve and back
   is not bit-exact (15 ms becomes 14.999999999999998), which changes state bytes nobody edited.

## State

1. `clap.state` save and load, starting with a version, so a later build reads an older state.
2. **A fixed layout.** openDAW stores the state as bytes and records an edit as the bytes that changed: with a
   fixed layout an edit changes a few bytes, so undo steps and collaboration updates stay tiny. Keep every value at
   its length (a float64 stays 8 bytes, no variable-length numbers). No counters, timestamps or animation in the
   state.
3. **Undo steps.** openDAW saves 250 ms after `clap.host_state->mark_dirty`, and each save is an undo step. Mark the
   state dirty at the end of a drag, not on every step, or a slow drag becomes several undo steps. A change that
   comes from openDAW itself (a project load, a delivered sample) is no edit and marks nothing.

## Latency, tail, sidechain

1. **Latency.** Answer `clap.latency`. When it changes (a lookahead, an attack that sets the delay), call
   `clap.host_latency->changed()`: openDAW reads it again right away, also while active.
2. **Tail.** Answer `clap.tail` for reverbs and delays, so the host keeps processing after the input stops.
3. **Sidechain.** A second input port without `CLAP_AUDIO_PORT_IS_MAIN`. openDAW hands an unconnected one over as
   silence (zeros, no constant mask), which cannot be told apart from a quiet one. Fall back to the main input
   while the sidechain is silent (Signalsmith Basics' Dynamics: the sidechain while it sounds and half a second
   after), or a detector that listens to silence closes a gate on everything.

## The data between page and plugin

Messages cross the worklet; every byte is copied, every message is an event in the page. Keep them few and small:

1. **The page asks, the plugin answers.** Poll from `requestAnimationFrame`, at most once a frame (every 33 ms is
   plenty for meters). The plugin never sends unasked, so a hidden page costs nothing.
2. **Binary, fixed layout, little endian**: a type byte, counts, then values at known offsets. No JSON, no strings,
   no CBOR on the hot path. float32 for readings, float64 only where exactness matters (parameter values).
3. **Aggregate between polls** on the audio thread: the peak since the last poll, the lowest gain, the loudest
   level, and reset them when read. The page sees every peak however seldom it asks.
4. **Heavy data only when new.** A spectrum goes out when the analyser has a new one and not otherwise (a count of 0
   in the message), downsampled to what the display draws.
5. **Only what changed**, where there is much: a snapshot once when the page says hello, then the changes (Helm,
   Note Lab). Small plugins can send their values in every answer (Signalsmith Basics: 36 to 132 bytes).
6. **Smooth in the page.** Ballistics, peak falls and needle inertia are drawn in the page from the raw readings;
   do not send animation frames.
7. **Both pages at once.** The window and the device panel can be open together and both receive every answer, so
   an answer must make sense to either.
8. Compare with the references: Signalsmith Basics sends 36 to 132 bytes per poll, the Analyser's spectrum 9.6 KB
   when a new one is ready; Note Lab an 80 byte snapshot, then 4 bytes per note.

## Speed

openDAW runs every device in one worklet with a few milliseconds per block for all of them.

1. **SIMD where it helps.** WebAssembly has 128-bit SIMD (`-msimd128`), which lets the compiler vectorise loops and
   maps SSE intrinsics through Emscripten's headers (or simde, as OB-Xf and Six Sines use). It is no free win:
   measure. In Signalsmith Basics `-msimd128` made the frequency shifter 2.1 times as fast (6.7 to 3.1 µs per
   128-frame block) and the analyser 1.6 times as slow (7.3 to 11.6 µs), the others the same. Where it differs per
   part, build those parts as their own translation units with their own flags.
2. **Measure in node**: a check that calls `process` in a loop (see `ports/<port>/check.mjs`) and times only the
   call, not the host filling the buffers. Microseconds per 128-frame block, a block is 2.7 ms at 48 kHz.
3. `-O3`, `-fno-exceptions`, `-fno-rtti` where the code allows; `-fno-math-errno` can help maths-heavy code (measure). Use float
   where double is not needed.
4. **Denormals.** WebAssembly cannot flush them to zero (no FTZ/DAZ), and decaying filters and reverb tails produce
   them, slow on some CPUs. Add a tiny offset, or flush small values yourself in feedback paths.
5. **Sleep in silence.** An instrument without voices, or an effect whose tail has ended, can skip its work (Helm's
   engine sleeps and wakes on a note or a change).
6. Keep the module small: it is downloaded once per version, but every user waits for it. Float tables instead of
   double, data generated at start rather than embedded, where the start stays fast.

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
   both receive everything the plugin sends.

Designing for 176 px:

- Lay out at 176 px high and 1:1 scale, then the panel is pixel exact. The window can show the same layout zoomed
  (Signalsmith Basics opens at 200 %: sharp on any screen since it is HTML and SVG, with canvases drawn at the
  zoom times `devicePixelRatio`).
- Fill the panel edge to edge: no outer margin, shadow or rounded corners, openDAW draws the device frame around
  it.
- Text below 8 px is hard to read; keep labels at 8 to 9 px and values in a readout or openDAW's value entry.
- At 176 px there is room for a header (about 34 px), a display and two rows of knobs about 30 px across. More
  controls than that want a wider panel, not a taller one.
- **Displays that hold still.** A display whose axes rescale with a parameter makes everything jump while the user
  turns the knob. Fix the axes to the parameter's whole range (a log axis covers a wide one) and let only the
  content move. Labels and selectors keep their width for every value (size a selector by its longest choice).

## Updating an existing bundle

A new build of a bundle that is in the catalogue reaches existing projects only when it keeps what they hold of it:

1. **The clap ids**, the **parameter ids** and the **parameter values** (ranges and meaning), or automation and
   MIDI learn point at the wrong thing. Taking over someone else's build means reading its ids and values from its
   module (a node host can list them).
2. **The state format**: the new build must load every state the old one wrote. Best: the same bytes for the same
   settings (Signalsmith Basics writes Signalsmith's CBOR byte for byte, `check.mjs` compares).
3. **Raise the version** in the descriptor.
4. In the catalogue the new archive **replaces** the old entry (the admin's Replace): its id goes into the new one's
   `supersedes`, the old file stays for projects that use it. The studio then shows an Update button on those
   devices; the user decides, nothing changes by itself.

## Testing

1. **The site** runs a bundle in openDAW's engine, as the studio does: `cd site && npm run dev`, then
   `http://localhost:8130/<id>?local` loads the archive the build script left in `build/`, with the device panel
   above the window (`panel: true` in `site/src/plugins.ts`). Feed it noise or a test tone, drag the knobs, check
   the parameter list and the messages. Import in the header loads any `.wclap.tar.gz`.
2. **A node check** (`ports/<port>/check.mjs`, copy one): a small CLAP host that runs the module without a browser,
   for timing, state bytes per edit, loading old states, latency and page traffic.
3. **openDAW Studio**: import the bundle on a track.

The plugin's `printf` and `clap.log` lines appear in the browser console. A trap stops the plugin, and openDAW
shows the device as failed; build with `-g` and without `-O3` for function names in the stack trace.

## Checklist

- [ ] WASI imports only, no allocation in `process`, parameter events at their sample
- [ ] the device panel: `opendaw:device-panel`, `opendaw.device-panel/1`, laid out at 176 px
- [ ] no surface of its own, the panel's colour, the studio's type (or a skin that keeps the rules)
- [ ] hover reported; no double-click, right-click or wheel of its own; no pointer lock
- [ ] a drag is one gesture and one undo step
- [ ] parameters in the units the editor shows (or the old bundle's), texts both ways
- [ ] a versioned state in a fixed layout, at most a few bytes per edit
- [ ] latency and tail answered, `host_latency->changed` when it moves, a silent sidechain handled
- [ ] polls at most once a frame, binary answers, readings aggregated, heavy data only when new
- [ ] measured: SIMD on where it helps, denormals handled
- [ ] displays that hold still, readouts inside the page
- [ ] tried on the site with `?local`, a node check, the version raised

## Sending us the bundle

Send the `.wclap.tar.gz`, the clap id(s) and the version, and raise the version with every build. We add it to
openDAW's catalogue.
