# Signalsmith Basics WebCLAP

[Signalsmith Basics](https://github.com/Signalsmith-Audio/basics) by Geraint Luff (Signalsmith Audio), MIT: Analyser,
Chorus, Crunch, Dynamics, Frequency Shifter, Limiter and Reverb as WebCLAP audio effects in one bundle. The effects are
upstream's, behind plain CLAP plugins with a page of their own (no JUCE, no STFX web helper).

## Taking over Signalsmith's build

Signalsmith publishes a WebCLAP build of six of them (`WebCLAP/examples`, `signalsmith-basics/basics.wclap.tar.gz`),
which openDAW's catalogue has. This build replaces it as an update, so it keeps everything a project holds of it:

- the clap ids (`uk.co.signalsmith.basics.<effect>`) and names (`[Basics] Chorus`),
- the parameter ids, names and values: a range parameter's value is its knob position (0..1), mapped through STFX's
  three points (low at 0, mid at 0.5, high at 1, the frequency shifter's shift through its own curve), a stepped one's
  is the step,
- the state: STFX's CBOR, an indefinite map with `version`, then per parameter its key and `{"value": …}` (float64, a
  step as an integer). The default states are Signalsmith's byte for byte, so its states load and stay loadable.

Dynamics is new (not in Signalsmith's build): the same scheme, its parameters in groups as upstream declares them
(`compressor`, `expander`, `gate`, `timing`), ids FNV-1a hashes of the paths, and a sidechain input. A host hands an
unconnected sidechain over as silence (openDAW does), so the detector follows the sidechain while it sounds and half a
second after, otherwise the main input.

## Layout

```
build.sh        builds module.wasm and copies the page and LICENSE into $OUT
plugin/         basics.cpp, the seven CLAP plugins around the effects
page/           index.html, the cards: #<effect> in a host, without one (or ?preview) all seven with made-up signals
check.mjs       node checks against Signalsmith's build: ids, defaults, state bytes, loading, traffic (26)
LICENSE         MIT (Signalsmith's and this port's)
```

`scripts/build-signalsmith-basics.sh` fetches the sources (`scripts/fetch-deps.py`: Basics, DSP, Linear, Hilbert IIR)
and `patches/signalsmith-basics-meters.patch`, which only exposes three readings for the displays (the chorus phase,
the dynamics detector level and gain, the limiter gain). Then it builds, packs and runs the checks.

## The page

A card per effect, 176 px high, in the design of [Device panel design](../../docs/device-panel-design.md) (this
page is its reference): openDAW's device panel shows it 1:1 (`opendaw.device-panel/1`, the width per effect from 246
to 382 px, declared as `opendaw:device-panel-width`), the window zoomed (100 to 300 %, opening at 200 %). The display
first, then the knobs (the main control large), Dynamics' and Reverb's minor parameters as rows, the Analyser's
resolution in its display's corner. Each has a display:

| Effect | Display |
|---|---|
| Chorus | the six delay taps sweeping across the depth, on the full 0 to 50 ms range |
| Crunch | the shaper's transfer curve at the drive and fuzz, the input peak on it |
| Dynamics | input to output level (threshold, ratio, knee, expander, gate), the detector level, the gain reduction |
| Frequency Shifter | the partials of a harmonic series and where the shift puts them, 0 Hz in the centre |
| Limiter | the gain reduction over the last seconds, hanging from 0 dB |
| Reverb | the impulse response on a fixed log time axis (2 ms to 30 s): dry, early reflections by the room, the tail by the decay time |
| Analyser | the Bark-scale spectrum of both channels |

The page reports the hovered parameter (`clap.param-hovered`), so openDAW's parameter menu (right-click) and value
entry (double-click) work on it. A drag is one gesture, the state is marked dirty at its end. The latency follows the
limiter's attack and the dynamics' lookahead (`clap.host_latency.changed`).
