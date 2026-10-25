/*
    Reads and writes the processor's private trajectory state and tap activity for the frames in osd_frames.h,
    as a friend (patches/open-spatial-delay-webclap.patch). The DSP side captures, the editor's stand-in applies.
*/

#pragma once

#include <cstring>

#include "PluginProcessor.h"
#include "osd_frames.h"

struct OsdWebclapAccess
{
    static_assert (osd_webclap::numObjects == OpenSpatialDelayProcessor::MAX_OBJECTS);
    static_assert (osd_webclap::numObjects == TrajectoryEngine::kMaxObjects);

    using NoiseFrame = TrajectoryEngine::RandomNoiseState[osd_webclap::numObjects];

    static void capture (const OpenSpatialDelayProcessor& processor, osd_webclap::MotionFrame& frame)
    {
        const auto& t = processor.trajectory;

        for (int i = 0; i < osd_webclap::numObjects; ++i)
        {
            frame.objects[i] = { t.finalAz_[i], t.finalEl_[i], t.finalDist_[i],
                                 t.baseAzimuth_[i], t.baseElevation_[i], t.baseDistance_[i],
                                 t.phase_[i], t.randomTime_[i], (float) t.prevShape_[i],
                                 t.active_[i].load (std::memory_order_relaxed) ? 1.0f : 0.0f,
                                 processor.tapActivityRMS[i].load (std::memory_order_relaxed) };
        }
    }

    static void apply (OpenSpatialDelayProcessor& processor, const osd_webclap::MotionFrame& frame)
    {
        auto& t = processor.trajectory;

        for (int i = 0; i < osd_webclap::numObjects; ++i)
        {
            const auto& object = frame.objects[i];
            t.finalAz_[i] = object.finalAz;
            t.finalEl_[i] = object.finalEl;
            t.finalDist_[i] = object.finalDist;
            t.baseAzimuth_[i] = object.baseAz;
            t.baseElevation_[i] = object.baseEl;
            t.baseDistance_[i] = object.baseDist;
            t.phase_[i] = object.phase;
            t.randomTime_[i] = object.randomTime;
            t.prevShape_[i] = (int) object.shape;
            t.active_[i].store (object.active > 0.5f, std::memory_order_relaxed);
            processor.tapActivityRMS[i].store (object.tapActivity, std::memory_order_relaxed);
        }
    }

    static const NoiseFrame& noise (const OpenSpatialDelayProcessor& processor) { return processor.trajectory.noise_; }

    static void applyNoise (OpenSpatialDelayProcessor& processor, const void* payload)
    {
        std::memcpy (processor.trajectory.noise_, payload, sizeof (NoiseFrame));
    }
};
