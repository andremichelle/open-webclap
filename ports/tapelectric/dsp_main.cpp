/*
    TapElectric as module.wasm: the processor, unchanged, wrapped by juce_webclap_clap.cpp. No editor code.

    PluginProcessor.cpp is compiled with TAPELECTRIC_HEADLESS (patches/tapelectric-headless.patch), which leaves out
    the editor. The processor needs JUCE's GUI modules all the same (AudioProcessorValueTreeState lives in
    juce_audio_processors), so this module links them; nothing reaches the windowing code, so the linker drops
    it and the module still imports only WASI.
*/

#include <juce_webclap/juce_webclap_clap.h>

#include <clap/plugin-features.h>

const juce::webclap::ClapPluginInfo& juce::webclap::getClapPluginInfo()
{
    static const char* const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_DISTORTION,
                                            CLAP_PLUGIN_FEATURE_STEREO, "tape", nullptr };

    static const ClapPluginInfo info {
        "com.aquanodemusic.tapelectric",
        "TapElectric",
        "aquanode",
        "https://github.com/aquanodemusic/aquanode-plugins/tree/main/effects/tapelectric",
        TAPELECTRIC_VERSION,
        "Tape deck atmosphere: power hum, tape noise, wow and saturation, the editor runs in the page",
        features,
        "/ui/index.html",
        940, // the editor's fixed size
        530
    };

    return info;
}
