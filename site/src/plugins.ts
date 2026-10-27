// The ported plugins. Each gets a page at /<id>. The bundles are the ones openDAW hosts (assets.opendaw.studio),
// found by clap id in its index (HostedIndex), or the site's own (bundled); in dev, ?local loads the archive the
// build script left in build/.
// No DOM here: vite.config.ts reads this list too.

export type PluginKind = "instrument" | "audio-fx"

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
        id: "glitchfest", name: "Glitchfest", kind: "audio-fx",
        description: "Glitch effect: twelve rows of stutter, tape, pitch and more, blocks drawn into patterns on the transport",
        clapId: "studio.opendaw.commons.glitchfest",
        archive: "build/glitchfest/glitchfest.wclap.tar.gz", build: "scripts/build-glitchfest.sh",
        credit: "https://github.com/andremichelle/opendaw-commons/tree/main/wclaps/glitchfest",
        source: "https://github.com/andremichelle/opendaw-commons/tree/main/wclaps/glitchfest", license: "MIT"
    }
]
