// The ported plugins. Each gets a page at /<id>. The bundles are the ones openDAW hosts (assets.opendaw.studio),
// found by clap id in its index (HostedIndex), or the site's own (bundled); in dev, ?local loads the archive the
// build script left in build/.
// No DOM here: vite.config.ts reads this list too.

export type PluginKind = "instrument" | "audio-fx" | "midi-fx"

export type PluginEntry = {
    id: string
    name: string
    kind: PluginKind
    description: string
    clapId: string // the plugin, and how openDAW's index finds its hosted bundle
    archive: string // the local build of the bundle, relative to the repository root
    build: string // the script that builds it
    credit: string // the original plugin
    source: string // this port
    license: string
    bundled?: string // served by the site itself, for a plugin that is not in openDAW's catalogue
}

// what a downloaded bundle is saved as
export const bundleFileName = (plugin: PluginEntry): string => `${plugin.id}.wclap.tar.gz`

const port = (name: string) => `https://github.com/andremichelle/open-webclap/tree/main/ports/${name}`

export const Plugins: ReadonlyArray<PluginEntry> = [
    {
        id: "obxf", name: "OB-Xf", kind: "instrument",
        description: "Virtual analog synth after the Oberheim OB-X, the continuation of OB-Xd",
        clapId: "org.surge-synth-team.OB-Xf",
        archive: "build/obxf/obxf.wclap.tar.gz", build: "scripts/build-obxf.sh",
        credit: "https://github.com/surge-synthesizer/OB-Xf", source: port("obxf"), license: "GPL-3.0-or-later"
    },
    {
        id: "six-sines", name: "Six Sines", kind: "instrument",
        description: "Small phase-modulation synth: six sine-based operators in a modulation matrix",
        clapId: "org.baconpaul.six-sines",
        archive: "build/six-sines/six-sines.wclap.tar.gz", build: "scripts/build-six-sines.sh",
        credit: "https://github.com/baconpaul/six-sines", source: port("six-sines"), license: "MIT (GPL-3.0 as built)"
    },
    {
        id: "ripplerx", name: "RipplerX", kind: "instrument",
        description: "Physically modeled percussion synth: mallet and noise exciters into coupled resonators",
        clapId: "com.tilr.ripplerx",
        archive: "build/ripplerx/ripplerx.wclap.tar.gz", build: "scripts/build-ripplerx.sh",
        credit: "https://github.com/tiagolr/ripplerx", source: port("ripplerx"), license: "GPL-3.0"
    },
    {
        id: "odin2", name: "Odin 2", kind: "instrument",
        description: "Polyphonic semi-modular synth: three oscillators, two filters, modulation matrix, effects",
        clapId: "com.thewavewarden.odin2",
        archive: "build/odin2/odin2.wclap.tar.gz", build: "scripts/build-odin2.sh",
        credit: "https://github.com/TheWaveWarden/odin2", source: port("odin2"), license: "GPL-3.0-or-later"
    },
    {
        id: "surge-xt", name: "Surge XT", kind: "instrument",
        description: "Hybrid synth: many oscillator, filter and effect types, deep modulation",
        clapId: "org.surge-synth-team.surge-xt",
        archive: "build/surge-xt/surge-xt.wclap.tar.gz", build: "scripts/build-surge-xt.sh",
        credit: "https://github.com/surge-synthesizer/surge", source: port("surge-xt"), license: "GPL-3.0-or-later"
    },
    {
        id: "coopiano", name: "coopiano", kind: "instrument",
        description: "Physically modelled piano: hammer, strings, soundboard, sympathetic resonance and room, no samples",
        clapId: "com.peterc.coopiano",
        archive: "build/coopiano/coopiano.wclap.tar.gz", build: "scripts/build-coopiano.sh",
        credit: "https://github.com/peterc/coopiano", source: port("coopiano"), license: "MIT"
    },
    {
        id: "dexed", name: "Dexed", kind: "instrument",
        description: "DX7 FM synthesizer: six operators, 32 algorithms, cartridge manager",
        clapId: "com.digital-suburban.dexed",
        archive: "build/dexed/dexed.wclap.tar.gz", build: "scripts/build-dexed.sh",
        credit: "https://github.com/asb2m10/dexed", source: port("dexed"), license: "GPL-3.0"
    },
    {
        id: "sc-dark", name: "Polarity SC Dark", kind: "audio-fx",
        description: "Spectral compressor with upward and downward compression",
        clapId: "de.polarity-music.polarity-sc-dark",
        archive: "build/sc-dark/polarity-sc-dark.wclap.tar.gz", build: "scripts/build-sc-dark.sh",
        credit: "https://github.com/polarity/plugin.nih.polarity-sc-dark", source: port("sc-dark"),
        license: "GPL-3.0-or-later"
    },
    {
        id: "open-spatial-delay", name: "OpenSpatialDelay", kind: "audio-fx",
        description: "Spatial delay: each echo placed in 3D space and moved along trajectories, binaural or stereo",
        clapId: "org.spatialmedialab.openspatialdelay",
        archive: "build/open-spatial-delay/open-spatial-delay.wclap.tar.gz",
        build: "scripts/build-open-spatial-delay.sh",
        credit: "https://github.com/Spatial-Media-Lab/OpenSpatialDelay", source: port("open-spatial-delay"),
        license: "GPL-3.0"
    },
    {
        id: "wet-chorus", name: "WetChorus", kind: "audio-fx",
        description: "Bucket-brigade stereo chorus after a 1970s guitar combo, a demo of openDAW's device panel",
        clapId: "studio.opendaw.commons.wet-chorus",
        archive: "build/wet-chorus/wet-chorus.wclap.tar.gz", build: "scripts/build-wet-chorus.sh",
        credit: "https://github.com/yonie/WetChorus", source: port("wet-chorus"), license: "MIT",
        bundled: "/bundled/wet-chorus.wclap.tar.gz"
    },
    {
        id: "note-lab", name: "Note Lab", kind: "midi-fx",
        description: "The reference note effect: passes, harmonizes, echoes or holds the notes it gets",
        clapId: "studio.opendaw.note-lab",
        archive: "build/note-lab/note-lab.wclap.tar.gz", build: "scripts/build-note-lab.sh",
        credit: port("note-lab"), source: port("note-lab"), license: "MIT",
        bundled: "/bundled/note-lab.wclap.tar.gz"
    },
    {
        id: "glitchfest", name: "Glitchfest", kind: "audio-fx",
        description: "Glitch effect: twelve rows of stutter, tape, pitch and more, blocks drawn into patterns on the transport",
        clapId: "studio.opendaw.commons.glitchfest",
        archive: "build/glitchfest/glitchfest.wclap.tar.gz", build: "scripts/build-glitchfest.sh",
        credit: "https://github.com/andremichelle/opendaw-commons/tree/main/wclaps/glitchfest",
        source: "https://github.com/andremichelle/opendaw-commons/tree/main/wclaps/glitchfest", license: "MIT"
    },
    {
        id: "zl-equalizer", name: "ZL Equalizer 2", kind: "audio-fx",
        description: "Dynamic equalizer: 24 bands, each static or dynamic, with match EQ, spectrum analyzer and loudness matching",
        clapId: "com.zlaudio.zlequalizer",
        archive: "build/zl-equalizer/zl-equalizer.wclap.tar.gz", build: "scripts/build-zl-equalizer.sh",
        credit: "https://github.com/ZL-Audio/ZLEqualizer", source: port("zl-equalizer"), license: "AGPL-3.0"
    },
    {
        id: "zl-compressor", name: "ZL Compressor", kind: "audio-fx",
        description: "Dynamic range processor with a sidechain equalizer, transfer curve and level history",
        clapId: "com.zlaudio.zlcompressor",
        archive: "build/zl-compressor/zl-compressor.wclap.tar.gz", build: "scripts/build-zl-compressor.sh",
        credit: "https://github.com/ZL-Audio/ZLCompressor", source: port("zl-compressor"), license: "AGPL-3.0"
    },
    {
        id: "zl-limiter", name: "ZL Limiter", kind: "audio-fx",
        description: "Lookahead limiter with true peak, oversampling, level history and loudness readouts",
        clapId: "com.zlaudio.zllimiter",
        archive: "build/zl-limiter/zl-limiter.wclap.tar.gz", build: "scripts/build-zl-limiter.sh",
        credit: "https://github.com/ZL-Audio/ZLLimiter", source: port("zl-limiter"), license: "AGPL-3.0"
    },
    {
        id: "zl-spectrum-equalizer", name: "ZL Spectrum Equalizer", kind: "audio-fx",
        description: "Spectrum equalizer with dynamic bands, spectrum analyzer and loudness matching",
        clapId: "com.zlaudio.zlspectrumequalizer",
        archive: "build/zl-spectrum-equalizer/zl-spectrum-equalizer.wclap.tar.gz", build: "scripts/build-zl-spectrum-equalizer.sh",
        credit: "https://github.com/ZL-Audio/ZLSpectrumEqualizer", source: port("zl-spectrum-equalizer"), license: "AGPL-3.0"
    },
    {
        id: "zl-splitter", name: "ZL Splitter", kind: "audio-fx",
        description: "Splitter: left/right, mid/side, low/high, transient/steady or peak/steady, one part on the output",
        clapId: "com.zlaudio.zlsplitter",
        archive: "build/zl-splitter/zl-splitter.wclap.tar.gz", build: "scripts/build-zl-splitter.sh",
        credit: "https://github.com/ZL-Audio/ZLSplitter", source: port("zl-splitter"), license: "AGPL-3.0"
    },
    {
        id: "zl-inflator", name: "ZL Inflator", kind: "audio-fx",
        description: "Saturation that inflates the signal's loudness, with a curve control and oversampling",
        clapId: "com.zlaudio.zlinflator",
        archive: "build/zl-inflator/zl-inflator.wclap.tar.gz", build: "scripts/build-zl-inflator.sh",
        credit: "https://github.com/ZL-Audio/ZLInflator", source: port("zl-inflator"), license: "GPL-3.0"
    },
    {
        id: "zl-warm", name: "ZL Warm", kind: "audio-fx",
        description: "Distortion and saturation with in and out meters",
        clapId: "com.zlaudio.zlwarm",
        archive: "build/zl-warm/zl-warm.wclap.tar.gz", build: "scripts/build-zl-warm.sh",
        credit: "https://github.com/ZL-Audio/ZLWarm", source: port("zl-warm"), license: "GPL-3.0"
    },
    {
        id: "zl-lmatch", name: "ZL Loudness Match", kind: "audio-fx",
        description: "Loudness matching: measures the gain that matches the output's loudness to a reference",
        clapId: "com.zlaudio.zllmatch",
        archive: "build/zl-lmatch/zl-lmatch.wclap.tar.gz", build: "scripts/build-zl-lmatch.sh",
        credit: "https://github.com/ZL-Audio/ZLLMatch", source: port("zl-lmatch"), license: "GPL-3.0"
    },
    {
        id: "zl-lmakeup", name: "ZL Loudness Makeup", kind: "audio-fx",
        description: "Loudness make-up: brings the input to the loudness of a reference on the sidechain",
        clapId: "com.zlaudio.zllmakeup",
        archive: "build/zl-lmakeup/zl-lmakeup.wclap.tar.gz", build: "scripts/build-zl-lmakeup.sh",
        credit: "https://github.com/ZL-Audio/ZLLMakeup", source: port("zl-lmakeup"), license: "GPL-3.0"
    }
]
