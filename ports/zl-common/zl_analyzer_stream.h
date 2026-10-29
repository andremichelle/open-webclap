/*
    The ZL plugins' analyzers across the split: the processor pushes the audio it analyses into a
    zldsp::analyzer::AnalyzerSenderBase (lock-free FIFOs), and the editor's worker loop pulls from them. In the
    WebCLAP ports the processor runs in module.wasm and the editor against a stand-in that never processes, so the
    DSP side drains its sender into frames and the UI side pushes them into the stand-in's sender, where the editor
    finds them as it would.

    Frame payload, little endian:
        u32 frames, f32 sampleRate, u8 sources, u8 encoding (1 = i16), u16 reserved
        per source: u8 kind: channels (1 to 127), 0 = not sent (the editor does not show it, it gets silence),
                    0x80 | n = the same samples as source n (ZL's side source copies the input while no sidechain is used)
        per sent source, per channel: f32 scale, frames * i16 (sample = i16 * scale)

    16 bits per sample with a scale per channel and frame: the quantisation noise sits about 100 dB below the
    channel's peak, under the analyzers' floor, at half the bytes of floats. The DSP side sends only while the page
    is open (PageExtension::update), so the FIFO fills and stops taking samples when nobody reads it, as it does
    upstream with the editor closed.
*/

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace zl_webclap
{
struct AnalyzerHeader
{
    uint32_t frames;
    float sampleRate;
    uint8_t sources, encoding;
    uint16_t reserved;
};

static_assert (sizeof (AnalyzerHeader) == 12);

constexpr uint8_t analyzerEncoding = 1, analyzerAlias = 0x80;

/** DSP side: what the sender's FIFO holds as one frame payload, at most maxFrames samples per channel (the
    oldest are dropped). Only the sources in mask (bit n = source n) are sent. Returns false if there is nothing to
    send. */
template <template <typename, std::size_t> class Sender, typename Float, std::size_t N>
bool drainAnalyzer (Sender<Float, N>& sender, std::vector<uint8_t>& out, int maxFrames = 1 << 14,
                    uint32_t mask = 0xffffffff)
{
    auto& fifo = sender.getAbstractFIFO();
    auto ready = fifo.getNumReady();

    if (ready <= 0)
        return false;

    if (ready > maxFrames)
    {
        (void) fifo.prepareToRead (ready - maxFrames);
        fifo.finishRead (ready - maxFrames);
        ready = maxFrames;
    }

    const auto& fifos = sender.getSampleFIFOs();
    const auto channels = sender.getNumChannels();
    const auto range = fifo.prepareToRead (ready);
    const auto frames = (std::size_t) ready;

    const auto same = [&] (std::size_t a, std::size_t b)
    {
        if (channels[a] != channels[b])
            return false;

        for (std::size_t ch = 0; ch < channels[a]; ++ch)
        {
            const auto* x = fifos[a][ch].data();
            const auto* y = fifos[b][ch].data();

            if (std::memcmp (x + range.start_index1, y + range.start_index1, (std::size_t) range.block_size1 * sizeof (float)) != 0
                || std::memcmp (x + range.start_index2, y + range.start_index2, (std::size_t) range.block_size2 * sizeof (float)) != 0)
                return false;
        }

        return true;
    };

    std::array<uint8_t, N> kinds {};
    std::size_t sent = 0;

    for (std::size_t i = 0; i < N; ++i)
    {
        if ((mask & (1u << i)) == 0 || channels[i] == 0)
            continue;

        kinds[i] = (uint8_t) channels[i];

        for (std::size_t j = 0; j < i; ++j)
        {
            if (kinds[j] != 0 && (kinds[j] & analyzerAlias) == 0 && same (i, j))
            {
                kinds[i] = (uint8_t) (analyzerAlias | j);
                break;
            }
        }

        if ((kinds[i] & analyzerAlias) == 0)
            sent += channels[i];
    }

    const AnalyzerHeader header { (uint32_t) frames, (float) sender.getSampleRate(), (uint8_t) N, analyzerEncoding, 0 };
    out.resize (sizeof (header) + N + sent * (sizeof (float) + frames * sizeof (int16_t)));
    auto* p = out.data();
    std::memcpy (p, &header, sizeof (header));
    p += sizeof (header);
    std::memcpy (p, kinds.data(), N);
    p += N;

    for (std::size_t i = 0; i < N; ++i)
    {
        if (kinds[i] == 0 || (kinds[i] & analyzerAlias) != 0)
            continue;

        for (std::size_t ch = 0; ch < channels[i]; ++ch)
        {
            const auto* source = fifos[i][ch].data();
            const std::span<const float> parts[] { { source + range.start_index1, (std::size_t) range.block_size1 },
                                                   { source + range.start_index2, (std::size_t) range.block_size2 } };
            float peak = 0.0f;

            for (const auto& part : parts)
                for (const auto x : part)
                    peak = std::max (peak, std::abs (x));

            const auto scale = peak > 0.0f && std::isfinite (peak) ? peak / 32767.0f : 1.0f;
            std::memcpy (p, &scale, sizeof (scale));
            p += sizeof (scale);

            for (const auto& part : parts)
            {
                for (const auto x : part)
                {
                    const auto q = (int16_t) std::lrint (std::clamp (x / scale, -32767.0f, 32767.0f));
                    std::memcpy (p, &q, sizeof (q));
                    p += sizeof (q);
                }
            }
        }
    }

    fifo.finishRead (ready);
    return true;
}

/** UI side: reads the header of a frame payload, false if it is not one. */
inline bool readAnalyzerHeader (const uint8_t* payload, std::size_t size, AnalyzerHeader& header)
{
    if (size < sizeof (header))
        return false;

    std::memcpy (&header, payload, sizeof (header));
    return header.encoding == analyzerEncoding && size >= sizeof (header) + header.sources;
}

/** UI side: pushes a frame into the stand-in's sender (process(), as the processor would). */
template <template <typename, std::size_t> class Sender, typename Float, std::size_t N>
void feedAnalyzer (Sender<Float, N>& sender, const uint8_t* payload, std::size_t size)
{
    AnalyzerHeader header;

    if (! readAnalyzerHeader (payload, size, header) || header.sources != N || header.frames == 0)
        return;

    const auto* kinds = payload + sizeof (header);
    const auto frames = (std::size_t) header.frames;
    const auto channels = sender.getNumChannels();
    const auto channelBytes = sizeof (float) + frames * sizeof (int16_t);
    std::array<const uint8_t*, N> data {}; // each sent source's first channel
    auto* p = kinds + N;

    for (std::size_t i = 0; i < N; ++i)
    {
        if (kinds[i] == 0)
            continue;

        if ((kinds[i] & analyzerAlias) != 0)
        {
            const auto j = (std::size_t) (kinds[i] & ~analyzerAlias);

            if (j >= i || data[j] == nullptr || channels[i] != channels[j])
                return;

            data[i] = data[j];
            continue;
        }

        if (kinds[i] != channels[i])
            return;

        data[i] = p;
        p += channels[i] * channelBytes;
    }

    if (p > payload + size)
        return;

    static std::vector<std::vector<Float>> buffers;
    static std::vector<Float*> pointers;
    std::size_t total = 0;

    for (std::size_t i = 0; i < N; ++i)
        total += channels[i];

    buffers.resize (total);
    pointers.resize (total);

    const auto maxBlock = std::max<std::size_t> (1, sender.getMaxNumSamples());

    for (std::size_t offset = 0; offset < frames; offset += maxBlock)
    {
        const auto block = std::min<std::size_t> (maxBlock, frames - offset);
        std::array<std::span<Float*>, N> spans;
        std::size_t index = 0;

        for (std::size_t i = 0; i < N; ++i)
        {
            for (std::size_t ch = 0; ch < channels[i]; ++ch, ++index)
            {
                auto& buffer = buffers[index];
                buffer.assign (block, Float (0));

                if (data[i] != nullptr)
                {
                    const auto* channel = data[i] + ch * channelBytes;
                    float scale;
                    std::memcpy (&scale, channel, sizeof (scale));
                    const auto* samples = channel + sizeof (scale) + offset * sizeof (int16_t);

                    for (std::size_t s = 0; s < block; ++s)
                    {
                        int16_t q;
                        std::memcpy (&q, samples + s * sizeof (q), sizeof (q));
                        buffer[s] = (Float) ((float) q * scale);
                    }
                }

                pointers[index] = buffer.data();
            }

            spans[i] = std::span<Float*> (pointers.data() + index - channels[i], channels[i]);
        }

        sender.process (spans, block);
    }
}
} // namespace zl_webclap
