// The ZL ports' analyzer frames (zl_analyzer_stream.h) for a device panel page: the audio the processor analyses,
// per source and channel, as floats. Copied into each ZL bundle's ui/ (scripts/zl-build.sh).
//
//     ZLAnalyzer.decode(view) -> {frames, sampleRate, sources: [null | [Float32Array per channel]]}
//
// view: the whole frame (the kit's 4-byte header first). A source not sent is null; an alias is its source's channels.
"use strict"
var ZLAnalyzer = (() => {
    const ALIAS = 0x80, SAMPLES = 0, SILENT = 1, AS_BEFORE = 2
    const decode = view => {
        const base = 4
        if (view.byteLength < base + 12) {return null}
        const frames = view.getUint32(base, true), sampleRate = view.getFloat32(base + 4, true)
        const count = view.getUint8(base + 8)
        const kinds = Array.from({length: count}, (_, index) => view.getUint8(base + 12 + index))
        const sources = new Array(count).fill(null)
        let at = base + 12 + count
        for (let index = 0; index < count; index++) {
            const kind = kinds[index]
            if (kind === 0 || (kind & ALIAS) !== 0) {continue}
            const channels = []
            for (let channel = 0; channel < kind; channel++) {
                if (at >= view.byteLength) {return null}
                const mode = view.getUint8(at++)
                if (mode === SAMPLES) {
                    if (at + 4 + frames * 2 > view.byteLength) {return null}
                    const scale = view.getFloat32(at, true), samples = new Float32Array(frames)
                    for (let frame = 0; frame < frames; frame++) {samples[frame] = view.getInt16(at + 4 + frame * 2, true) * scale}
                    channels.push(samples)
                    at += 4 + frames * 2
                } else if (mode === AS_BEFORE && channel > 0) {
                    channels.push(channels[channel - 1])
                } else if (mode === SILENT) {
                    channels.push(new Float32Array(frames))
                } else {
                    return null
                }
            }
            sources[index] = channels
        }
        kinds.forEach((kind, index) => {
            if ((kind & ALIAS) !== 0) {sources[index] = sources[kind & ~ALIAS]}
        })
        return {frames, sampleRate, sources}
    }
    // the peak of a source over all its channels (dB), -Infinity for silence or none
    const peakDb = channels => {
        if (channels === null || channels === undefined) {return -Infinity}
        let peak = 0
        for (const samples of channels) {for (const sample of samples) {peak = Math.max(peak, Math.abs(sample))}}
        return 20 * Math.log10(peak)
    }
    return {decode, peakDb}
})()
