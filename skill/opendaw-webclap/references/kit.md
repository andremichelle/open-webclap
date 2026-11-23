# The openDAW WebCLAP kit

`source/kit/opendaw_kit.h` turns a parameter table and a `Dsp` struct into a complete CLAP plugin for openDAW:
descriptor and features, parameters with texts both ways, sample-accurate events, a fixed-layout versioned state,
gestures as one undo step, the hovered parameter for openDAW's right-click menu and double-click value entry, the
device panel (176 px), an optional window with content of its own, latency and tail, the page's messages and,
for MIDI effects, a well-formed note stream. `source/page/opendaw-kit.js` builds the page from the plugin's
description. You write the sound and, optionally, a display.

## plugin.cpp

```cpp
#include "opendaw_kit.h"
using namespace opendaw;

enum : clap_id {FREQ, RES, MIX, PARAM_COUNT};          // ids 0, 1, 2... in table order

static constexpr const char *MODES[] = {"Low", "Band", "High"};
static constexpr Param PARAMS[PARAM_COUNT] = {
    Param::knob(FREQ, "Freq", 20.0, 20000.0, 1000.0, Unit::Hertz, Curve::Exponential),
    Param::knob(RES, "Res", 0.0, 100.0, 30.0, Unit::Percent),
    Param::knob(MIX, "Mix", 0.0, 100.0, 100.0, Unit::Percent)
};
static constexpr Info INFO = {"studio.opendaw.commons.<slug>", "Name", "Author", "1.0.0", "Description.",
                              Kind::AudioEffect, "filter", 196};

struct Dsp : DspBase {
    void activate(double sampleRate, uint32_t maxFrames, const double *params) {...}
    void process(Block &block) {...}
};

OPENDAW_PLUGIN(Dsp, INFO, PARAMS)
```

### Parameters

| Constructor | Host value | Page |
|---|---|---|
| `Param::knob(id, name, min, max, default, unit, curve)` | the value in its unit | a knob |
| `Param::integer(id, name, min, max, default, unit)` | a whole number | a knob in steps |
| `Param::choice(id, name, labels, count, default)` | the step index 0..count-1 | the steps as words |

Units: `None, Percent (0..100), Decibel, Hertz, Milliseconds, Seconds, Semitones, Cents`. Curves (the page's knob
only): `Linear`, `Exponential` (frequencies, needs min > 0), `Squared` (times). `OFF_ON` is a ready two-step label
list for a switch: `Param::choice(BYPASS, "Freeze", OFF_ON, 2, 0)`. A range symmetric around zero (-24..24) draws
its arc from the top. Texts come from the kit ("1.20 kHz", "-6.0 dB", "120 ms", "+7 st", "50%", "-inf dB" at a
dB minimum of -60 or lower) and parse back ("4k", "1.5 s").

INFO: clap id, name, vendor, version, description, kind, one category feature, the panel width at 176 px and,
only for a plugin with a window, the window's width and height (see The window).
`new.mjs` fills id (always `studio.opendaw.commons.<slug>`, never change it), name (as in meta.json), vendor (the
author) and description; you set the kind's feature and the width. The feature is a plain string that openDAW's
device browser sorts by, one of: `"synthesizer"`, `"drum"`, `"sampler"`, `"keys"`, `"bass"`, `"granular"`,
`"compressor"`, `"limiter"`, `"gate"`, `"equalizer"`, `"filter"`, `"reverb"`, `"delay"`, `"distortion"`,
`"saturation"`, `"lo-fi"`, `"chorus"`, `"tremolo"`, `"pitch"`, `"panning"`, `"analyzer"`, `"mastering"`,
`"arpeggiator"`, `"velocity"`, `"generative"`, `"sequencer"`, `"utility"`. Raise the version (1.0.0 → 1.0.1) with every
build the user takes into a project.

**Edit plugin.cpp in place.** Keep its first lines and the INFO values new.mjs filled in; replace the parameter table,
the feature, the width and the Dsp. Any number of parameters is fine: from eight on, the minor ones become parameter
rows on the page.

### The Dsp

Derive from `DspBase` and write only what you need:

| Method | When |
|---|---|
| `activate(sampleRate, maxFrames, params)` | before processing; allocate here, start smoothers at `params[id]` |
| `deactivate()` | after processing; rarely needed |
| `reset()` | stop, seek, bypass: silence everything now (voices, delay lines, filters), no allocation |
| `changed(id, value)` | a parameter changed (automation, page, state load), before the samples it applies to |
| `process(Block &block)` | render `block.frames` (1 to 128) frames |
| `noteOn(note, notes)`, `noteOff(note, notes)` | instruments and MIDI effects, at their sample (between blocks) |
| `midi(data, notes)` | instruments: other MIDI (pitch bend `0xE0`, CC `0xB0`, sustain is CC 64) |
| `latency() const` | frames of delay the plugin adds (lookahead); the kit tells the host when it changes |
| `tail() const` | audio effects: frames the output rings on after the input stops (`UINT32_MAX` endless) |
| `meters(out, capacity)` | floats for the page's display, read and reset per poll (`Peak::take`) |

The kit constructs the Dsp once per plugin instance (default member initialisers run, the constructor takes no
arguments) and destroys it with the plugin: free what `activate` allocated in a destructor (`~Dsp()`) and before
allocating again in the next `activate`. Never copy it.

`Block`: `frames`, `input[2]` (audio effects, both channels always set, never the same memory as the output, so
reading the input after writing the output is safe), `output[2]` (effects write it, instruments ADD to it, it starts
at zero), `params` (by id, in units, constant in the block), `transport` (`playing`, `tempo`,
`beats` at the block's first frame in quarter notes, `barStart`, `numerator`, `denominator`), `notes`, `sampleRate`.
The kit splits blocks at every event, so `process` never sees a change inside its block. The output is checked for
NaN (replaced by silence) and clipped at ±4.

Helpers: `dbToGain`, `gainToDb`, `keyToHz` (key 69 = 440 Hz; openDAW names key 60 C3), `flush(x)` (denormals to 0),
`Smoothed` (`prepare(sampleRate, ms, initial)`, `set(target)`, `next()` per sample), `Peak` (`feed`, `take`).

### MIDI effects

`notes.on(key, velocity, channel, frame)` and `notes.off(key, channel, frame)`, frame relative to the current call
(0 in `noteOn`/`noteOff`, the block's frame in `process`). The kit sorts the output, re-strikes a key that sounds
already (off, then on) and sends a key's note off with the last of its sources, so the stream is always well formed.
`notes.releaseAll(frame)` ends everything (a mode change). Remember what each incoming key started and end exactly
that in `noteOff`, whatever the parameters say by then (see the template). Other MIDI passes through by itself. On
reset only forget held notes, the host ends what it got. Tempo-synced patterns read `block.transport.tempo` and count
frames, never calls (`noteOn` only learns which keys are held, `process` plays them at their frames). An arpeggiator:

```cpp
// members: int held[16], heldCount (sorted keys, kept in noteOn/noteOff), step, playing = -1;
//          double untilNext, untilOff (frames, across blocks); reset() sets them back
void process(Block &block) {
    const double framesPerStep = (params[RATE] == 0 ? 0.25 : 0.5) * 60.0 * block.sampleRate / block.transport.tempo;
    for (uint32_t frame = 0; frame < block.frames; frame++) {
        if (playing >= 0 && (untilOff -= 1.0) <= 0.0) {block.notes.off(playing, 0, frame); playing = -1;}
        if ((untilNext -= 1.0) <= 0.0) {
            untilNext += framesPerStep;
            if (heldCount == 0) {step = 0; continue;}
            const int key = held[step++ % heldCount];
            if (playing >= 0) {block.notes.off(playing, 0, frame);}
            block.notes.on(key, 0.8, 0, frame);
            playing = key;
            untilOff = framesPerStep * params[GATE] / 100.0;
        }
    }
}
```

To lock steps to the song's grid instead, start a step when `floor((block.transport.beats + frame * tempo /
(60 * sampleRate)) / stepBeats)` changes while `block.transport.playing`.

### Instruments

A fixed voice array (8 to 16), a free voice or the oldest released one for a new note, a voice freed when its level
falls under -100 dB, nothing rendered without voices (the instrument sleeps). Velocity scales the level. Retrigger
the same key in place. Release time comes from parameters at release, not at note on.

### Recipes

- `meters()` is the only way data reaches the page's display, beside the parameter values. Send what changes in the
  sound (peaks, an LFO's value, a gain reduction), at most 64 floats, and let the page animate between them.
- Gains and frequencies: `Smoothed` per sample, 10 to 30 ms, set in `changed()`. An envelope is its own per-sample
  ramp (linear attack, exponential release), it needs no `Smoothed`.
- A one-pole split (tilt EQ, shelves, a tone control): `low += a * (in - low)`, `high = in - low`, with
  `a = 1 - exp(-2π f / sampleRate)` from a smoothed frequency; tilt = `low * dbToGain(-tilt/2) + high * dbToGain(tilt/2)`.
- Chorus, flanger, vibrato: an LFO phase advanced per sample (`phase += rate / sampleRate`, wrapped), a delay line
  read at `centre + depth * sin(2π phase)` with linear interpolation, the right channel a quarter cycle later.
  `meters()` can send `sin` and `cos` of the phase, the page recovers it with `atan2`.
- Polyphony: sum the voices times about `0.25` (eight voices) so chords stay below 0 dBFS; scale by velocity.
  A per-voice filter envelope moves the cutoff in octaves: `hz = cutoff * exp2(amount * envelope * 5)`, clamped below
  `0.45 * sampleRate`.
- Delay lines: allocate the maximum length in `activate`, clear in `reset`, read with linear interpolation, flush the
  feedback. A delay time change glides (smooth the read position) or it zips.
- Filters: TPT state variable filter or biquads computed from smoothed values; recompute coefficients per sample only
  when the value moves.
- Distortion: `tanh`, oversampling only if aliasing is audible; keep the output level close to the input's.
- Reverb, delay: answer `tail()`.
- Randomness: your own xorshift seeded in `activate`, never `rand()` with time.
- Keep the output around 0 dBFS at default settings; check.mjs warns above +3.5 dB.

## The page

`source/page/index.html` loads `opendaw-page/panel.js` (openDAW's knobs and choices), `opendaw-kit.js`, and calls:

```js
OpenDAWKit.start()                                          // everything automatic
OpenDAWKit.start({
    layout: [{large: 0}, {knobs: [[1, "M"], [2, "M"], [3, "M"]]}],   // columns by parameter id (optional)
    display: {width: 96, draw: (context, width, height, kit) => {...}}   // optional, left of the controls
})
```

The automatic layout follows openDAW's device design: the first knob large, the rest three to a column (four as a
square), a choice with one or two knobs stacked in one column, more than seven knobs as parameter rows. Explicit
columns: `{large: id}`, `{knobs: [[id, "L"|"M"|"S"], ...], rows: 2|3}`, `{stack: [[id, size], ...]}`, `{choices:
[ids]}`, `{labels: [ids or null]}` (rows, null a gap). Put related parameters in one column (Attack and Release),
levels last.

The display is `display.width` px wide and 160 px high (the panel's 176 minus its padding), in CSS pixels, cleared
before every `draw`: `kit.value(id)` (units),
`kit.text(id)`, `kit.meters` (floats from `meters()`), `kit.time` (seconds), `kit.panel` (true in the device panel).
Use `OpenDAWPanel.trace(context, [[x, y], ...], {fill: baselineY, width: 1.25, alpha: 0.85})` (an open line in the
display blue, with `fill` a fade from the line down to `baselineY`), `OpenDAWPanel.gridLine(context, x0, y0, x1, y1,
true)` (a dashed reference line), `OpenDAWPanel.caption(context, text, x, y, align)` (7 px scale numbers),
`OpenDAWPanel.dot(context, x, y)` (2 px), and `OpenDAWPanel.ink(alpha)` for any other drawing. Rules: no background or frame, axes fixed to the parameter's whole range (a knob moves the
curve, never the scale), peaks fall back smoothly in the page, nothing drawn that carries no information.

The kit already keeps openDAW's rules on the page: the hovered parameter is reported before any press, a drag is one
gesture and one undo step, no wheel, no double-click and no right-click of its own (those are openDAW's), ctrl+click
ignored (macOS's right-click), values in tabular digits at the width of their longest text, the plugin's older answers
never overwrite a value being changed, polls at most every 33 ms. When you add your own controls to a page, keep
those rules: never listen to `wheel`, `dblclick`, `contextmenu` or keys, never `preventDefault` a press or lock the
pointer, keep popups inside the page (it is clipped at its edges), and report hover for every control that stands for
a parameter.

## The window (optional)

A plugin is its device panel. Only when the user wants something the panel cannot hold (a large display, a detailed
editor) does it get a window, and the window then shows that, never the panel's controls again. Give its size in INFO
after the panel width, and build its content in the page:

```cpp
static constexpr Info INFO = {"studio.opendaw.commons.<slug>", "Name", "Author", "1.0.0", "Description.",
                              Kind::AudioEffect, "analyzer", 196, 640, 360};   // panel 196, window 640 x 360
```

```js
OpenDAWKit.start({
    display: {...},                                  // the panel as before
    window: {
        build: (root, kit) => {                      // root: a div of the window's size, the panel's colour
            const canvas = document.createElement("canvas")
            canvas.style.cssText = "position: absolute; inset: 8px 8px 60px 8px; width: calc(100% - 16px); height: calc(100% - 68px)"
            root.append(canvas)
            const controls = document.createElement("div")
            controls.className = "group"
            controls.style.cssText = "position: absolute; left: 8px; bottom: 4px"
            root.append(controls)
            kit.layout([{knobs: [[3, "S"]]}], controls)  // a fine setting only the window has
            const context = canvas.getContext("2d")
            kit.onFrame(() => {
                const {width, height} = OpenDAWPanel.fit(canvas, context, kit.zoom())
                // draw the large view from kit.value(id) and kit.meters
            })
        }
    }
})
```

The window opens from a "Big View" button the kit puts on the panel (in the display's corner, or as the last column)
and from openDAW's device menu. It keeps its aspect and zooms from 75 to 200 %. In the window, `kit.control(id,
size)`, `kit.row(id)` and `kit.layout(columns, element)` give openDAW's controls for any parameter, with hover, gestures
and undo as on the panel, and `kit.onFrame(callback)` runs every frame with the latest values and meters. Everything
the window changes must be a parameter, so it is automatable and saved.

## The state

The kit saves every parameter: `ODKS`, version, count, then (id, value) pairs of 16 bytes. An edit changes 8 bytes,
so openDAW's undo steps and collaboration updates stay tiny. Unknown ids are skipped on load and missing ones keep
their default, so a later version with more parameters loads older projects. Everything a user sets must be a
parameter; there is no other state.
