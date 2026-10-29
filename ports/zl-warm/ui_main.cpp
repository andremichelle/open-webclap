/*
    ZLWarm's editor as ui.wasm (zl-common/zl_ui_main.h), with its glue: the meters from the DSP side go into the
    stand-in controller's meters, which the meter panel paints; a click that resets a meter's maximum goes back as
    controls. The editor keeps its UI settings (style, window size) in a second APVTS the processor does not save,
    so they are not host state.
*/

#include "zl_warm_meters.h"

#define ZL_NO_SETTINGS_STATE 1
#include "../zl-common/zl_ui_main.h"

namespace frames = zl_warm_webclap;

namespace
{
frames::Controls resets {};
frames::Controls sentResets {};
bool resetsSent = false;
float shownMax[2] {}; // the summed maxima written last, per meter: anything else is the editor's reset

float sumOfMax (zlMeter::SingleMeter<float>& meter)
{
    auto sum = 0.0f;

    for (auto& v : meter.getmaxPeak())
        sum += v.load();

    return sum;
}
} // namespace

void zlPortInit (ZLApp& zl)
{
    shownMax[0] = sumOfMax (zl.processor->getController().getInMeter());
    shownMax[1] = sumOfMax (zl.processor->getController().getOutMeter());

    zl.bridge->onPluginFrame = [&zl] (juce::uint8 type, const juce::uint8* payload, size_t size)
    {
        if (type != frames::meters || size != sizeof (frames::Meters))
            return;

        frames::Meters meters;
        std::memcpy (&meters, payload, sizeof (meters));
        auto& controller = zl.processor->getController();

        // A maximum from before the editor's last reset would bring the old value back for a frame
        frames::apply (controller.getInMeter(), meters.in, meters.numChannels, meters.in.resets == resets.inResets);
        frames::apply (controller.getOutMeter(), meters.out, meters.numChannels, meters.out.resets == resets.outResets);
        shownMax[0] = sumOfMax (controller.getInMeter());
        shownMax[1] = sumOfMax (controller.getOutMeter());
    };
}

void zlPortFrame (ZLApp& zl)
{
    auto& controller = zl.processor->getController();

    // SingleMeterPanel::mouseDown resets the stand-in's meter: its maxima are no longer what the DSP side sent
    if (sumOfMax (controller.getInMeter()) != shownMax[0])
    {
        ++resets.inResets;
        shownMax[0] = sumOfMax (controller.getInMeter());
    }

    if (sumOfMax (controller.getOutMeter()) != shownMax[1])
    {
        ++resets.outResets;
        shownMax[1] = sumOfMax (controller.getOutMeter());
    }

    if (! resetsSent || std::memcmp (&resets, &sentResets, sizeof (resets)) != 0)
    {
        resetsSent = true;
        sentResets = resets;
        zl.send (frames::controls, &resets, sizeof (resets));
    }
}
