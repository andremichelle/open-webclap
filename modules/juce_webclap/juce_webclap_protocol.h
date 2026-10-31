/*
    juce_webclap: the split-UI message protocol, shared by the UI side (juce_webclap_bridge.h) and the DSP side
    (juce_webclap_clap.cpp). See PLAN.md, "Message protocol".

    Frames are little endian: u8 type, u8 flags, u16 reserved, payload.

        hello     UI -> DSP   u32 protocol version
        snapshot  DSP -> UI   u32 count, count * (u32 clapId, f64 value), u32 stateSize, state bytes
        param     both        u32 clapId, f64 value (normalised 0..1)
        gesture   UI -> DSP   u32 clapId, u8 1 = begin, 0 = end
        state     both        opaque blob (AudioProcessor::getStateInformation)
        stream    DSP -> UI   u16 streamId, samples
        resize    UI -> DSP   u32 width, u32 height (logical pixels)
        midi      UI -> DSP   1 to 3 MIDI bytes (notes from the editor's keyboard), played in the next process
        show      UI -> DSP   no payload: the page (a device panel) asks for the plugin's window (clap.gui request_show)

    Types from firstPluginType on are the port's own: what an editor needs beyond parameters and state
    (ports/ripplerx, through PageExtension and ProcessorBridge), or a whole protocol for plugins whose editor
    does not talk through AudioProcessor parameters (ports/six-sines). The header has no JUCE dependency, so DSP modules
    built without JUCE use it too.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace juce::webclap
{

namespace protocol
{
    using uint8 = std::uint8_t;
    using uint16 = std::uint16_t;
    using uint32 = std::uint32_t;

    enum Type : uint8
    {
        hello    = 1,
        snapshot = 2,
        param    = 3,
        gesture  = 4,
        state    = 5,
        stream   = 6,
        resize   = 7,
        midi     = 8,
        show     = 9
    };

    constexpr uint8 firstPluginType = 64;

    constexpr uint32 version = 1;

    /** Set on param/state frames the UI sends to seed a DSP side that has no values yet (empty snapshot). */
    constexpr uint8 flagSync = 1;

    struct Writer
    {
        explicit Writer (uint8 type, uint8 flags = 0) { u8 (type); u8 (flags); u16 (0); }

        void u8 (uint8 v)   { bytes.push_back (v); }
        void u16 (uint16 v) { put (&v, sizeof (v)); }
        void u32 (uint32 v) { put (&v, sizeof (v)); }
        void f32 (float v)  { put (&v, sizeof (v)); }
        void f64 (double v) { put (&v, sizeof (v)); }

        // wasm is little endian, so memcpy produces the wire format
        void put (const void* data, size_t size)
        {
            const auto* p = static_cast<const uint8*> (data);
            bytes.insert (bytes.end(), p, p + size);
        }

        std::vector<uint8> bytes;
    };

    struct Reader
    {
        Reader (const void* data, size_t size) : p (static_cast<const uint8*> (data)), end (p + size) {}

        bool has (size_t n) const { return (size_t) (end - p) >= n; }

        template <typename T>
        T get()
        {
            T v {};

            if (has (sizeof (T)))
            {
                std::memcpy (&v, p, sizeof (T));
                p += sizeof (T);
            }
            else
            {
                p = end;
                ok = false;
            }

            return v;
        }

        const uint8* p;
        const uint8* end;
        bool ok = true;
    };
}

} // namespace juce::webclap
