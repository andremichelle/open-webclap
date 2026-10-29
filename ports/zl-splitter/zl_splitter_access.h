/*
    Reads and writes the controller's private editor settings for the frames in zl_splitter_frames.h, as a friend
    (patches/zl-splitter-webclap.patch). The editor's stand-in reads the controls, the DSP side applies them.
*/

#pragma once

#include "PluginProcessor.hpp"
#include "zl_splitter_frames.h"

namespace zlp
{
struct ZLWebclapAccess
{
    static constexpr auto relaxed = std::memory_order::relaxed;

    /** UI side: the controls as the editor left them. */
    template <typename FloatType>
    static zl_splitter_webclap::Controls controls (Controller<FloatType>& c)
    {
        return { (uint8_t) c.analyzer_on_.load (relaxed) };
    }

    /** DSP side: the editor's controls, through the controller's own setters. */
    template <typename FloatType>
    static void apply (Controller<FloatType>& c, const zl_splitter_webclap::Controls& controls)
    {
        if ((controls.analyzerOn != 0) != c.analyzer_on_.load (relaxed))
            c.setAnalyzerOn (controls.analyzerOn != 0);
    }
};
} // namespace zlp
