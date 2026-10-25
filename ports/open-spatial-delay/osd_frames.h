/*
    OpenSpatialDelay's port frames (juce_webclap_protocol.h, firstPluginType and up). Everything else is the kit's
    generic path: parameters, and state for the configuration the editor keeps outside parameters (algorithm, HRTF
    profile, output and input format), which the processor reports as a non-parameter state change.

        motion  DSP -> UI   MotionFrame: per object the animated position, the trajectory state the spatial map
                            draws and the tap activity. Trajectories advance in processBlock, which the editor's
                            stand-in never runs. Sent whole to a page that just opened, or when most values moved.
        delta   DSP -> UI   The MotionFrame values that changed since the last frame, (u8 index, f32 value) each, as
                            MotionFrame::values counts them. Up to 30 times a second, only when something moved.
        noise   DSP -> UI   TrajectoryEngine::RandomNoiseState[12]: the random trajectory's path, which the
                            editor previews. Rolled on the DSP side, sent when it changes.
        preset  UI -> DSP   i32 preset the editor loaded. The DSP side loads it itself (loadPreset), which also
                            resets the trajectories, as a native host would.
*/

#pragma once

#include <juce_webclap/juce_webclap_protocol.h>

namespace osd_webclap
{
enum Frame : juce::webclap::protocol::uint8
{
    motion = juce::webclap::protocol::firstPluginType,
    noise,
    preset,
    delta
};

constexpr int numObjects = 12;

struct ObjectMotion
{
    float finalAz, finalEl, finalDist;
    float baseAz, baseEl, baseDist;
    float phase, randomTime;
    float shape, active, tapActivity;
};

union MotionFrame
{
    ObjectMotion objects[numObjects];
    float values[numObjects * sizeof (ObjectMotion) / sizeof (float)];
};

static_assert (sizeof (ObjectMotion) % sizeof (float) == 0 && sizeof (MotionFrame::values) <= 256 * sizeof (float));

constexpr size_t deltaEntrySize = 1 + sizeof (float);
} // namespace osd_webclap
