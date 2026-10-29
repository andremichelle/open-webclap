/*
    Reads and writes the controller's private display values and editor settings for the frames in
    zl_equalizer_frames.h, as a friend (patches/zl-equalizer-webclap.patch). The DSP side captures meters and
    applies controls, the editor's stand-in the other way round.
*/

#pragma once

#include "PluginProcessor.hpp"
#include "zl_equalizer_frames.h"

#if ! ZL_WEBCLAP_HEADLESS
 #include "PluginEditor.hpp"
#endif

namespace zlp
{
struct ZLWebclapAccess
{
    static_assert (zl_equalizer_webclap::numBands == kBandNum);

    static constexpr auto relaxed = std::memory_order::relaxed;

    static void capture (Controller& c, zl_equalizer_webclap::Meters& meters)
    {
        auto& m = meters.m;

        for (size_t i = 0; i < kBandNum; ++i)
        {
            m.gains[i] = (float) c.current_gains_[i].load (relaxed);
            m.sideLoudness[i] = (float) c.dynamic_side_loudness_display_[i].load (relaxed);
            m.thresholds[i] = (float) c.learned_thresholds_[i].load (relaxed);
            m.knees[i] = (float) c.learned_knees_[i].load (relaxed);
        }

        m.displayedGain = (float) c.displayed_gain_.load (relaxed);
        m.lufsDiff = (float) c.loudness_matcher_.getDiff();
    }

    static void apply (Controller& c, const zl_equalizer_webclap::Meters& meters)
    {
        const auto& m = meters.m;

        for (size_t i = 0; i < kBandNum; ++i)
        {
            c.current_gains_[i].store (m.gains[i], relaxed);
            c.dynamic_side_loudness_display_[i].store (m.sideLoudness[i], relaxed);
            c.learned_thresholds_[i].store (m.thresholds[i], relaxed);
            c.learned_knees_[i].store (m.knees[i], relaxed);
        }

        c.displayed_gain_.store (m.displayedGain, relaxed);
        c.loudness_matcher_.setDiff (m.lufsDiff);
    }

    /** UI side: the controls as the editor left them. Consumes the editor's solo gain reset. */
    static zl_equalizer_webclap::Controls controls (Controller& c)
    {
        return { (uint32_t) c.solo_whole_idx_.load (relaxed),
                 (float) c.solo_gain_db_.load (relaxed),
                 (uint8_t) c.match_bypass_on_.load (relaxed),
                 (uint8_t) c.loudness_matcher_on_.load (relaxed),
                 (uint8_t) c.editor_on_.load (relaxed),
                 (uint8_t) c.to_reset_solo_gain_.check(),
                 0 };
    }

#if ! ZL_WEBCLAP_HEADLESS
    /** UI side: the analyzer sources the editor reads, as its settings and panels say. The FFT reads the ones
        switched on, the output meter pre and post, the match panel pre and side. */
    static uint32_t analyzerMask (PluginProcessor& p, juce::AudioProcessorEditor* editor)
    {
        const auto on = [&p] (const char* id) { return p.parameters_NA_.getRawParameterValue (id)->load (relaxed) > .5f; };
        const auto meter = on (zlstate::PMeterDisplayON::kID);
        auto match = false;

        if (auto* zl = dynamic_cast<PluginEditor*> (editor))
            match = (double) zl->base_.getPanelProperty (zlgui::PanelSettingIdx::kMatchPanel) > .5;

        return (on (zlstate::PFFTPreON::kID) || meter || match ? 1u : 0u)
             | (on (zlstate::PFFTPostON::kID) || meter ? 2u : 0u)
             | (on (zlstate::PFFTSideON::kID) || match ? 4u : 0u);
    }
#endif

    /** DSP side: the editor's controls, through the controller's own setters. */
    static void apply (Controller& c, const zl_equalizer_webclap::Controls& controls)
    {
        if (controls.soloWholeIdx != c.solo_whole_idx_.load (relaxed))
            c.setSoloWholeIdx (controls.soloWholeIdx);

        if (controls.resetSoloGain)
            c.resetSoloGain();
        else if (controls.soloGain != (float) c.solo_gain_db_.load (relaxed))
            c.setSoloGain (controls.soloGain);

        if ((controls.matchBypass != 0) != c.match_bypass_on_.load (relaxed))
            c.setMatchBypassON (controls.matchBypass != 0);

        if ((controls.loudnessMatch != 0) != c.loudness_matcher_on_.load (relaxed))
            c.setLoudnessMatchON (controls.loudnessMatch != 0);

        if ((controls.editorOn != 0) != c.editor_on_.load (relaxed))
            c.setEditorON (controls.editorOn != 0);
    }
};
} // namespace zlp
