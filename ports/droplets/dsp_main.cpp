/*
    Droplets as module.wasm: the processor, unchanged, wrapped by juce_webclap_clap.cpp. No editor code.

    PluginProcessor.cpp is compiled with DROPLETS_HEADLESS (patches/droplets-headless.patch), which leaves out
    the editor. The processor needs JUCE's GUI modules all the same (AudioProcessorValueTreeState lives in
    juce_audio_processors), so this module links them; nothing reaches the windowing code, so the linker drops
    it and the module still imports only WASI.
*/

#include <juce_webclap/juce_webclap_clap.h>

#include <clap/plugin-features.h>

const juce::webclap::ClapPluginInfo& juce::webclap::getClapPluginInfo()
{
    static const char* const features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                            CLAP_PLUGIN_FEATURE_STEREO, "physical modeling", nullptr };

    static const ClapPluginInfo info {
        "com.aquanodemusic.droplets",
        "Droplets",
        "aquanode",
        "https://github.com/aquanodemusic/aquanode-plugins/tree/main/synths/droplets",
        DROPLETS_VERSION,
        "Stochastic water bubble synthesizer, the editor runs in the page",
        features,
        "/ui/index.html",
        550, // the editor's fixed size
        520
    };

    return info;
}
