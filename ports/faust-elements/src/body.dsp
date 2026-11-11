// The effect bus of sletz/faust-elements' dsp/elements_poly.dsp (MIT), verbatim, for the WebCLAP port: el.body,
// what Part::Process does after the voices — the stereo mix of the space control, the soft limit, the reverb.
// Three inputs (the voices' raw, center, side sums), two outputs (main, aux; the module's OUT and AUX).
// Same program name as voice.dsp, so the controls share the upstream addresses; compiled to its own class
// (body_gen.cpp, class ElementsBody) in its own translation unit. Needs faust-rs, single precision, 32 kHz.
declare name "elements_poly";
el = library("elements.lib");

process = el.body;
