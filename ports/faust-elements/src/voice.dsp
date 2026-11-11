// The voice of sletz/faust-elements' dsp/elements_poly.dsp (MIT), verbatim, for the WebCLAP port: the same
// program name, so the controls keep the upstream addresses (/elements_poly/exciter/bow, ...), and the same
// process expression: el.voice at the key's frequency plus the modulation (pitch offset) input, the velocity as
// the strength, the gate, and the resonator model. The effect bus (el.body) is compiled separately (body.dsp) and
// summed by the wrapper, as the web AudioWorklet (web/elements-processor.js) sums the voices into it.
// Needs faust-rs (elements.lib uses ondemand), compiled single precision at 32 kHz, the module's rate.
declare name "elements_poly";
el = library("elements.lib");

freq = hslider("freq", 220, 8, 12000, 0.001);
gain = hslider("gain", 0.5, 0, 1, 0.001);
gate = button("gate");

process = el.voice(el.note2f(el.freq2note(freq) + el.modulation), gain, gate, el.model, 0.0, 0.0);
