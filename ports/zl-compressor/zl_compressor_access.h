/*
    Reads and writes the controllers' private analyzer switches and solo state for the frames in
    zl_compressor_frames.h, as a friend (patches/zl-compressor-webclap.patch). The DSP side captures the status and
    applies controls, the editor's stand-in the other way round.
*/

#pragma once

#include "PluginProcessor.hpp"
#include "zl_compressor_frames.h"

namespace zlp
{
struct ZLWebclapAccess
{
    static constexpr auto relaxed = std::memory_order::relaxed;

    static void capture (PluginProcessor& p, zl_compressor_webclap::Status& s)
    {
        s.lufsDiff = p.getCompressController().getLUFSMatcherDiff();
        s.sampleRate = (float) p.getAtomicSampleRate();
    }

    /** UI side: the LUFS difference into the stand-in's matcher (the sample rate is the port's to follow). */
    static void apply (PluginProcessor& p, const zl_compressor_webclap::Status& s)
    {
        p.getCompressController().lufs_matcher_.setDiff (s.lufsDiff);
    }

    /** UI side: the controls as the editor left them. Consumes the editor's solo gain reset. */
    static zl_compressor_webclap::Controls controls (PluginProcessor& p)
    {
        auto& compress = p.getCompressController();
        auto& equalize = p.getEqualizeController();
        return { (uint32_t) equalize.getSoloBand(),
                 (float) equalize.getSoloGain(),
                 (uint8_t) compress.mag_analyzer_on_.load (relaxed),
                 (uint8_t) equalize.getFFTAnalyzerON(),
                 (uint8_t) compress.lufs_matcher_on_.load (relaxed),
                 (uint8_t) equalize.to_reset_solo_gain_.check(),
                 magAnalyzerMask (p) };
    }

    /** UI side: the magnitude analyzer sources the editor reads. The peak, reduction and meter displays read pre,
        compressed and post always, the side chain only for its curve. */
    static uint32_t magAnalyzerMask (PluginProcessor& p)
    {
        const auto side = p.na_parameters_.getRawParameterValue (zlstate::PSideChainCurveDisplay::kID)->load (relaxed) > .5f;
        return (1u << CompressController::kAnalyzerPreStream) | (1u << CompressController::kAnalyzerCompressedStream)
             | (1u << CompressController::kAnalyzerPostStream)
             | (side ? 1u << CompressController::kAnalyzerSideChainStream : 0u);
    }

    /** DSP side: the page went away. What the editor's destructors clear on its stand-in (analyzers, LUFS
        matcher, solo) is cleared here. */
    static void closed (PluginProcessor& p)
    {
        auto& compress = p.getCompressController();
        auto& equalize = p.getEqualizeController();
        compress.setMagAnalyzerOn (false);
        compress.setLUFSMatcherOn (false);
        equalize.setFFTAnalyzerON (false);

        if (equalize.getSoloBand() != kBandNum)
        {
            equalize.resetSoloGain();
            equalize.setSoloBand (kBandNum);
        }
    }

    /** DSP side: the editor's controls, through the controllers' own setters. */
    static void apply (PluginProcessor& p, const zl_compressor_webclap::Controls& controls)
    {
        auto& compress = p.getCompressController();
        auto& equalize = p.getEqualizeController();

        if ((controls.magAnalyzerOn != 0) != compress.mag_analyzer_on_.load (relaxed))
            compress.setMagAnalyzerOn (controls.magAnalyzerOn != 0);

        if ((controls.fftAnalyzerOn != 0) != equalize.getFFTAnalyzerON())
            equalize.setFFTAnalyzerON (controls.fftAnalyzerOn != 0);

        if ((controls.lufsMatcherOn != 0) != compress.lufs_matcher_on_.load (relaxed))
            compress.setLUFSMatcherOn (controls.lufsMatcherOn != 0);

        if (controls.soloBand <= kBandNum && controls.soloBand != equalize.getSoloBand())
            equalize.setSoloBand (controls.soloBand);

        if (controls.resetSoloGain)
            equalize.resetSoloGain();
        else if (controls.soloGain != (float) equalize.getSoloGain())
            equalize.setSoloGain (controls.soloGain);
    }
};
} // namespace zlp
