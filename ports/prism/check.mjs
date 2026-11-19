// Prism in node: a minimal CLAP host checks the eight analyzers: their descriptors and device panels, the audio
// passing through unchanged, every scope's frames (the binary layouts page/backend.ts reads), two pages at once, the
// settings in the state, and the time a block and a frame take. Usage: node check.mjs <module.wasm>

import assert from "node:assert/strict"
import {readFileSync} from "node:fs"

const SAMPLE_RATE = 48000
const BLOCK = 128
const VALTYPE = {i32: 0x7f, i64: 0x7e, f32: 0x7d, f64: 0x7c}
const SCOPES = ["spectrum", "oscilloscope", "vectorscope", "spectrogram", "vumeter", "lufsmeter", "waveform", "waterfall"]
const PAGE = {POLL: 1, READY: 2, CONFIG: 3, BYE: 5}
const PLUGIN = {FRAME: 1, RESTORE: 2}

const trampoline = (params, results, fn) => {
    const name = text => [text.length, ...[...text].map(letter => letter.charCodeAt(0))]
    const section = (id, bytes) => [id, bytes.length, ...bytes]
    const type = [1, 0x60, params.length, ...params.map(kind => VALTYPE[kind]), results.length, ...results.map(kind => VALTYPE[kind])]
    const bytes = new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        ...section(1, type), ...section(2, [1, ...name("e"), ...name("f"), 0x00, 0x00]), ...section(7, [1, ...name("f"), 0x00, 0x00])])
    return new WebAssembly.Instance(new WebAssembly.Module(bytes), {e: {f: fn}}).exports.f
}

class Module {
    static async load(path) {
        const module = await WebAssembly.compile(readFileSync(path))
        const imports = {}
        let memory = null
        const wasi = {
            clock_time_get: (_id, _precision, pointer) => {
                new DataView(memory.buffer).setBigUint64(pointer, process.hrtime.bigint(), true)
                return 0
            },
            random_get: (pointer, length) => {
                crypto.getRandomValues(new Uint8Array(memory.buffer, pointer, length))
                return 0
            }
        }
        for (const entry of WebAssembly.Module.imports(module)) {
            (imports[entry.module] ??= {})[entry.name] = entry.module === "wasi_snapshot_preview1" && wasi[entry.name]
                || (() => {throw new Error(`${entry.module}.${entry.name} called`)})
        }
        const instance = await WebAssembly.instantiate(module, imports)
        memory = instance.exports.memory
        instance.exports._initialize?.()
        return new Module(instance.exports)
    }

    constructor(exports) {
        this.exports = exports
        this.table = exports.__indirect_function_table
        const entry = exports.clap_entry.value
        assert.ok(this.fn(entry + 12)(this.string("/")), "entry init")
        this.factory = this.fn(entry + 20)(this.string("clap.plugin-factory"))
    }

    get view() {return new DataView(this.exports.memory.buffer)}
    get bytes() {return new Uint8Array(this.exports.memory.buffer)}
    u32(pointer) {return this.view.getUint32(pointer, true)}
    fn(pointer) {return this.table.get(this.u32(pointer))}
    malloc(size) {return this.exports.malloc(size)}
    cstr(pointer) {
        const bytes = this.bytes
        let end = pointer
        while (bytes[end] !== 0) {end++}
        return new TextDecoder().decode(bytes.subarray(pointer, end))
    }
    string(text) {
        const encoded = new TextEncoder().encode(text)
        const pointer = this.malloc(encoded.length + 1)
        this.bytes.set(encoded, pointer)
        this.bytes[pointer + encoded.length] = 0
        return pointer
    }
    install(pointer, params, results, fn) {
        const index = this.table.grow(1)
        this.table.set(index, trampoline(params, results, fn))
        this.view.setUint32(pointer, index, true)
    }
    descriptors() {
        const count = this.fn(this.factory)(this.factory)
        return Array.from({length: count}, (_, index) => {
            const descriptor = this.fn(this.factory + 4)(this.factory, index)
            const features = []
            for (let pointer = this.u32(descriptor + 44); this.u32(pointer) !== 0; pointer += 4) {features.push(this.cstr(this.u32(pointer)))}
            return {id: this.cstr(this.u32(descriptor + 12)), name: this.cstr(this.u32(descriptor + 16)), features}
        })
    }
}

class Plugin {
    constructor(module, id) {
        this.module = module
        this.sent = []
        this.dirty = 0
        this.latencyChanges = 0
        this.hovered = []
        this.resizes = []
        this.out = []
        this.createHost()
        const m = module
        this.plugin = m.fn(m.factory + 8)(m.factory, this.host, m.string(id))
        assert.ok(this.plugin, `create ${id}`)
        assert.ok(m.fn(this.plugin + 8)(this.plugin), "init")
        const extension = name => m.fn(this.plugin + 40)(this.plugin, m.string(name))
        this.params = extension("clap.params")
        this.state = extension("clap.state")
        this.latency = extension("clap.latency")
        this.tail = extension("clap.tail")
        this.webview = extension("clap.webview/3")
        this.audioPorts = extension("clap.audio-ports")
        this.panel = extension("opendaw.device-panel/1")
        this.gui = extension("clap.gui")
        this.createProcess()
    }

    createHost() {
        const m = this.module
        const host = m.malloc(48)
        const extensions = new Map()
        const extension = (name, size) => {
            const pointer = m.malloc(size)
            extensions.set(name, pointer)
            return pointer
        }
        const strings = ["prism check", "open-webclap", "", "1"].map(text => m.string(text))
        m.view.setUint32(host, 1, true)
        m.view.setUint32(host + 4, 2, true)
        m.view.setUint32(host + 8, 0, true)
        m.view.setUint32(host + 12, 0, true)
        strings.forEach((pointer, index) => m.view.setUint32(host + 16 + index * 4, pointer, true))
        m.install(host + 32, ["i32", "i32"], ["i32"], (_host, id) => extensions.get(m.cstr(id)) ?? 0)
        for (const offset of [36, 40, 44]) {m.install(host + offset, ["i32"], [], () => {})}
        const params = extension("clap.params", 12)
        m.install(params, ["i32", "i32"], [], () => {})
        m.install(params + 4, ["i32", "i32", "i32"], [], () => {})
        m.install(params + 8, ["i32"], [], () => {})
        m.install(extension("clap.state", 4), ["i32"], [], () => this.dirty++)
        m.install(extension("clap.latency", 4), ["i32"], [], () => this.latencyChanges++)
        m.install(extension("clap.webview/3", 4), ["i32", "i32", "i32"], ["i32"], (_host, pointer, size) => {
            this.sent.push(m.bytes.slice(pointer, pointer + size))
            return 1
        })
        m.install(extension("clap.param-hovered/1", 4), ["i32", "i32"], [], (_host, id) => this.hovered.push(id >>> 0))
        const gui = extension("clap.gui", 20)
        m.install(gui, ["i32"], [], () => {})
        m.install(gui + 4, ["i32", "i32", "i32"], ["i32"], (_host, width, height) => {
            this.resizes.push([width, height])
            return 1
        })
        for (const offset of [8, 12]) {m.install(gui + offset, ["i32"], ["i32"], () => 1)}
        m.install(gui + 16, ["i32", "i32"], [], () => {})
        this.host = host
    }

    createProcess() {
        const m = this.module
        this.process = m.malloc(40)
        this.slots = m.malloc(48 * 64)
        this.count = 0
        this.inEvents = m.malloc(12)
        m.install(this.inEvents + 4, ["i32"], ["i32"], () => this.count)
        m.install(this.inEvents + 8, ["i32", "i32"], ["i32"], (_list, index) => index < this.count ? this.slots + index * 48 : 0)
        this.outEvents = m.malloc(8)
        m.install(this.outEvents + 4, ["i32", "i32"], ["i32"], (_list, pointer) => {
            this.out.push({type: m.view.getUint16(pointer + 10, true), id: m.u32(pointer + 16)})
            return 1
        })
        this.channels = [0, 1, 2, 3].map(() => m.malloc(BLOCK * 4))
        this.outputs = [0, 1].map(() => m.malloc(BLOCK * 4))
        const buffer = (channels, pointers) => {
            const array = m.malloc(channels * 4)
            pointers.forEach((pointer, index) => m.view.setUint32(array + index * 4, pointer, true))
            const audio = m.malloc(24)
            m.view.setUint32(audio, array, true)
            m.view.setUint32(audio + 4, 0, true)
            m.view.setUint32(audio + 8, channels, true)
            return audio
        }
        this.inputBuffers = m.malloc(48)
        const main = buffer(2, this.channels.slice(0, 2)), side = buffer(2, this.channels.slice(2))
        m.bytes.copyWithin(this.inputBuffers, main, main + 24)
        m.bytes.copyWithin(this.inputBuffers + 24, side, side + 24)
        this.outputBuffer = buffer(2, this.outputs)
    }

    activate() {
        const m = this.module
        assert.ok(m.fn(this.plugin + 16)(this.plugin, SAMPLE_RATE, 1, BLOCK), "activate")
        assert.ok(m.fn(this.plugin + 24)(this.plugin), "start_processing")
    }

    // one block; input(channel, frame) fills the inputs, events are [time, id, value] parameter changes
    run(input, events = []) {
        const m = this.module
        for (let channel = 0; channel < 4; channel++) {
            for (let frame = 0; frame < BLOCK; frame++) {m.view.setFloat32(this.channels[channel] + frame * 4, input(channel, frame), true)}
        }
        this.count = events.length
        events.forEach(([time, id, value], index) => {
            const at = this.slots + index * 48
            m.bytes.fill(0, at, at + 48)
            m.view.setUint32(at, 48, true)
            m.view.setUint32(at + 4, time, true)
            m.view.setUint16(at + 10, 5, true)
            m.view.setUint32(at + 16, id, true)
            m.view.setInt32(at + 24, -1, true)
            m.view.setInt16(at + 28, -1, true)
            m.view.setInt16(at + 30, -1, true)
            m.view.setInt16(at + 32, -1, true)
            m.view.setFloat64(at + 40, value, true)
        })
        const p = this.process
        m.view.setBigInt64(p, 0n, true)
        m.view.setUint32(p + 8, BLOCK, true)
        m.view.setUint32(p + 12, 0, true)
        m.view.setUint32(p + 16, this.inputBuffers, true)
        m.view.setUint32(p + 20, this.outputBuffer, true)
        m.view.setUint32(p + 24, 2, true)
        m.view.setUint32(p + 28, 1, true)
        m.view.setUint32(p + 32, this.inEvents, true)
        m.view.setUint32(p + 36, this.outEvents, true)
        const status = m.fn(this.plugin + 36)(this.plugin, p)
        this.count = 0
        assert.equal(status, 1, "process continues")
        return [0, 1].map(channel => Float32Array.from({length: BLOCK}, (_, frame) => m.view.getFloat32(this.outputs[channel] + frame * 4, true)))
    }

    paramInfos() {
        const m = this.module
        const info = m.malloc(2048)
        return Array.from({length: m.fn(this.params)(this.plugin)}, (_, index) => {
            assert.ok(m.fn(this.params + 4)(this.plugin, index, info))
            const view = m.view
            return {id: view.getUint32(info, true), flags: view.getUint32(info + 4, true), name: m.cstr(info + 12),
                min: view.getFloat64(info + 1296, true), max: view.getFloat64(info + 1304, true), default: view.getFloat64(info + 1312, true)}
        })
    }

    value(id) {
        const m = this.module
        const out = m.malloc(8)
        assert.ok(m.fn(this.params + 8)(this.plugin, id, out))
        return m.view.getFloat64(out, true)
    }

    text(id, value) {
        const m = this.module
        const text = m.malloc(256)
        assert.ok(m.fn(this.params + 12)(this.plugin, id, value, text, 256))
        return m.cstr(text)
    }

    fromText(id, text) {
        const m = this.module
        const out = m.malloc(8)
        if (!m.fn(this.params + 16)(this.plugin, id, m.string(text), out)) {return null}
        return m.view.getFloat64(out, true)
    }

    save() {
        const m = this.module
        const chunks = []
        const stream = m.malloc(8)
        m.install(stream + 4, ["i32", "i32", "i64"], ["i64"], (_stream, pointer, size) => {
            chunks.push(m.bytes.slice(pointer, pointer + Number(size)))
            return size
        })
        assert.ok(m.fn(this.state)(this.plugin, stream), "state save")
        return Buffer.concat(chunks)
    }

    load(bytes) {
        const m = this.module
        let at = 0
        const stream = m.malloc(8)
        m.install(stream + 4, ["i32", "i32", "i64"], ["i64"], (_stream, pointer, size) => {
            const count = Math.min(Number(size), bytes.length - at)
            m.bytes.set(bytes.subarray(at, at + count), pointer)
            at += count
            return BigInt(count)
        })
        return m.fn(this.state + 4)(this.plugin, stream)
    }

    receive(bytes) {
        const m = this.module
        const pointer = m.malloc(bytes.length)
        m.bytes.set(bytes, pointer)
        return m.fn(this.webview + 8)(this.plugin, pointer, bytes.length)
    }
}

let checks = 0
const check = (name, fn) => {
    fn()
    checks++
    console.log(`ok ${name}`)
}

const module = await Module.load(process.argv[2])
const descriptors = module.descriptors()

const pageMessage = (type, page, extra = new Uint8Array(0), at = 8) => {
    const bytes = new Uint8Array(Math.max(8, at + extra.length))
    const view = new DataView(bytes.buffer)
    view.setUint8(0, type)
    view.setUint32(4, page, true)
    bytes.set(extra, at)
    return bytes
}
const config = (page, kind, payload, persist = false) => {
    const json = new TextEncoder().encode(JSON.stringify(payload))
    const extra = new Uint8Array(4 + json.length)
    extra[0] = kind
    extra[1] = persist ? 1 : 0
    extra.set(json, 4)
    return pageMessage(PAGE.CONFIG, page, extra)
}

// a frame's fields after the 8 byte header, as backend.ts reads them
class Reader {
    constructor(bytes) {
        this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
        this.at = 8
    }
    f32() {const value = this.view.getFloat32(this.at, true); this.at += 4; return value}
    u32() {const value = this.view.getUint32(this.at, true); this.at += 4; return value}
    floats() {
        const count = this.u32()
        const values = Array.from({length: count}, (_, index) => this.view.getFloat32(this.at + index * 4, true))
        this.at += count * 4
        return values
    }
    get done() {return this.at === this.view.byteLength}
}

const sine = (frequency, amplitude = 0.5) => {
    let time = 0
    return (channel, frame) => {
        if (channel === 0 && frame === 0) {time += BLOCK}
        return channel < 2 ? amplitude * Math.sin(2 * Math.PI * frequency * (time - BLOCK + frame) / SAMPLE_RATE) * (channel === 1 ? 0.5 : 1) : 0
    }
}

// a page that started, configured and played: its frames (the plugin's messages to this page)
const framesOf = (plugin, page) => plugin.sent.filter(bytes => bytes[0] === PLUGIN.FRAME && new DataView(bytes.buffer).getUint32(4, true) === page)
const play = (plugin, input, blocks) => {for (let block = 0; block < blocks; block++) {plugin.run(input)}}
const poll = (plugin, page) => {
    plugin.sent.length = 0
    assert.ok(plugin.receive(pageMessage(PAGE.POLL, page)))
    const frames = framesOf(plugin, page)
    assert.equal(frames.length, 1, "one frame a poll")
    return frames[0]
}

check("eight analyzers, each its device panel at the width it declares, no window", () => {
    assert.deepEqual(descriptors.map(d => d.id), SCOPES.map(scope => `studio.opendaw.prism.${scope}`))
    for (const descriptor of descriptors) {
        assert.ok(descriptor.features.includes("audio-effect") && descriptor.features.includes("analyzer"), descriptor.id)
        assert.ok(descriptor.features.includes("opendaw:device-panel"), descriptor.id)
        const declared = descriptor.features.find(feature => feature.startsWith("opendaw:device-panel-width:"))
        const plugin = new Plugin(module, descriptor.id)
        const m = module
        const width = m.malloc(4)
        assert.ok(m.fn(plugin.panel + 4)(plugin.plugin, 176, width))
        assert.equal(Number(declared.split(":").pop()), m.u32(width), descriptor.id)
        const uri = m.malloc(64)
        m.fn(plugin.panel)(plugin.plugin, uri, 64)
        assert.equal(m.cstr(uri), `/index.html?panel#${descriptor.id.split(".").pop()}`)
        m.fn(plugin.webview)(plugin.plugin, uri, 64)
        assert.equal(m.cstr(uri), `/index.html?panel#${descriptor.id.split(".").pop()}`, "the webview's page is the panel's")
        assert.equal(plugin.gui, 0, "no window")
    }
})

check("the audio passes through unchanged", () => {
    const plugin = new Plugin(module, descriptors[0].id)
    plugin.activate()
    const input = sine(441)
    for (let block = 0; block < 4; block++) {
        const out = plugin.run(input)
        const view = module.view
        for (let channel = 0; channel < 2; channel++) {
            for (let frame = 0; frame < BLOCK; frame++) {
                assert.equal(out[channel][frame], view.getFloat32(plugin.channels[channel] + frame * 4, true))
            }
        }
    }
})

const PAGE_A = 0x1234567, PAGE_B = 0x7654321
const started = (scope, settings = {}) => {
    const plugin = new Plugin(module, `studio.opendaw.prism.${scope}`)
    plugin.activate()
    plugin.receive(config(PAGE_A, 0, settings))
    plugin.receive(pageMessage(PAGE.READY, PAGE_A))
    return plugin
}

check("spectrum: a 1 kHz sine peaks at 1 kHz, mid, side and channel max at fftSize / 2", () => {
    const plugin = started("spectrum", {fftSize: 4096, smoothing: 0})
    play(plugin, sine(1000), 80)
    const read = new Reader(poll(plugin, PAGE_A))
    assert.equal(read.f32(), SAMPLE_RATE)
    assert.equal(read.u32(), 1, "has data")
    read.u32(); read.f32(); read.f32()
    const mid = read.floats(), side = read.floats(), max = read.floats()
    assert.ok(read.done)
    assert.equal(mid.length, 2048)
    assert.equal(side.length, 2048)
    assert.equal(max.length, 2048)
    const peak = mid.indexOf(Math.max(...mid))
    assert.ok(Math.abs(peak * SAMPLE_RATE / 4096 - 1000) < 2 * SAMPLE_RATE / 4096, `peak at bin ${peak}`)
})

check("oscilloscope: a window of samples once warmed up, the pitch found", () => {
    const plugin = started("oscilloscope", {pitchLock: true})
    play(plugin, sine(220), 80)
    const read = new Reader(poll(plugin, PAGE_A))
    assert.equal(read.f32(), SAMPLE_RATE)
    const pitch = read.f32()
    const samples = read.floats()
    assert.ok(read.done)
    assert.ok(samples.length > 1000, `${samples.length} samples`)
    assert.ok(Math.abs(pitch - 220) < 22, `pitch ${pitch}`)
})

check("vectorscope: x and y, or the multiband points", () => {
    for (const multiband of [false, true]) {
        const plugin = started("vectorscope", {multiband})
        play(plugin, sine(300), 40)
        const read = new Reader(poll(plugin, PAGE_A))
        read.f32()
        assert.equal(read.u32(), multiband ? 1 : 0)
        const count = read.u32()
        assert.ok(count > 0)
        if (multiband) {assert.ok(read.floats().length >= count)} else {
            assert.equal(read.floats().length, count)
            assert.equal(read.floats().length, count)
        }
        assert.ok(read.done)
    }
})

check("spectrogram: columns of the page's row count after its native config", () => {
    const plugin = started("spectrogram")
    plugin.receive(config(PAGE_A, 1, {fftSize: 2048, rowCount: 100, minFrequency: 20, maxFrequency: 20000, scrollSpeed: 2,
        clarityMode: "normal", scaleMode: "log", orientation: "horizontal", sampleRate: 48000}))
    play(plugin, sine(1000), 200)
    const read = new Reader(poll(plugin, PAGE_A))
    read.f32()
    assert.equal(read.u32(), 100)
    const columns = read.u32()
    assert.ok(columns > 0, "columns")
    assert.equal(read.floats().length, columns * 100)
    assert.equal(read.floats().length, columns * 100)
    assert.ok(read.done)
    const next = new Reader(poll(plugin, PAGE_A))
    next.f32(); next.u32()
    assert.equal(next.u32(), 0, "only new columns")
})

check("vu and loudness meters: their readings rise with a signal", () => {
    for (const [scope, fields] of [["vumeter", 8], ["lufsmeter", 12]]) {
        const plugin = started(scope)
        play(plugin, sine(1000), 200)
        const read = new Reader(poll(plugin, PAGE_A))
        const values = Array.from({length: fields}, () => read.f32())
        assert.ok(read.done)
        assert.equal(values[0], SAMPLE_RATE)
        assert.ok(values[1] > -20, `${scope} ${values[1]}`)
    }
})

check("waveform: mono or stereo column summaries", () => {
    for (const mode of ["mono", "stereo"]) {
        const plugin = started("waveform", {mode, scrollSpeed: 1})
        play(plugin, sine(100), 100)
        const read = new Reader(poll(plugin, PAGE_A))
        read.f32()
        assert.equal(read.u32(), mode === "stereo" ? 1 : 0)
        const columns = read.u32()
        assert.ok(columns > 0)
        assert.equal(read.floats().length, columns * (mode === "stereo" ? 10 : 5))
        assert.ok(read.done)
    }
})

check("waterfall: ridges of the page's columns", () => {
    const plugin = started("waterfall", {fftSize: 2048, historySeconds: 5, smoothing: 0.5, tiltDbPerOctave: 2, scaleMode: "log"})
    plugin.receive(config(PAGE_A, 1, {revision: 3, ridges: 16, columns: 128}))
    play(plugin, sine(1000), 400)
    const read = new Reader(poll(plugin, PAGE_A))
    assert.equal(read.f32(), SAMPLE_RATE)
    assert.equal(read.u32(), 3, "revision")
    assert.equal(read.u32(), 128)
    assert.ok(read.f32() > 0.9, "audio seconds")
    const levels = read.floats(), ages = read.floats(), frequencies = read.floats()
    assert.ok(read.done)
    assert.equal(frequencies.length, 128)
    assert.ok(ages.length > 1)
    assert.equal(levels.length, ages.length * 128)
})

check("two pages at once: each its own engine and frames, both fed the same audio", () => {
    const plugin = started("spectrogram")
    plugin.receive(pageMessage(PAGE.READY, PAGE_B))
    plugin.receive(config(PAGE_A, 1, {fftSize: 2048, rowCount: 100, clarityMode: "normal"}))
    plugin.receive(config(PAGE_B, 1, {fftSize: 2048, rowCount: 40, clarityMode: "normal"}))
    play(plugin, sine(500), 200)
    const a = new Reader(poll(plugin, PAGE_A)), b = new Reader(poll(plugin, PAGE_B))
    a.f32(); b.f32()
    assert.equal(a.u32(), 100)
    assert.equal(b.u32(), 40)
    assert.equal(a.u32(), b.u32(), "the same columns")
})

check("the settings: a change of the user's is the state, marks it dirty and goes to the other pages", () => {
    const plugin = started("spectrum")
    const settings = {fftSize: 8192, smoothing: 0.5, displayMode: "fill"}
    plugin.sent.length = 0
    plugin.receive(config(PAGE_A, 0, settings, true))
    assert.equal(plugin.dirty, 1)
    const restore = plugin.sent.find(bytes => bytes[0] === PLUGIN.RESTORE)
    const view = new DataView(restore.buffer)
    assert.equal(view.getUint32(4, true), 0, "to every page")
    assert.equal(view.getUint32(8, true), PAGE_A, "from page A")
    assert.deepEqual(JSON.parse(new TextDecoder().decode(restore.subarray(12))), settings)
    plugin.receive(config(PAGE_A, 0, settings, true))
    assert.equal(plugin.dirty, 1, "no change, not dirty")
    plugin.receive(config(PAGE_A, 0, {...settings, fftSize: 1024}, false))
    assert.equal(plugin.dirty, 1, "a restore's echo is no edit")
    const state = plugin.save()
    const loaded = new Plugin(module, "studio.opendaw.prism.spectrum")
    assert.ok(loaded.load(state))
    assert.equal(loaded.save().toString("hex"), state.toString("hex"))
    const sent = loaded.sent.find(bytes => bytes[0] === PLUGIN.RESTORE)
    assert.deepEqual(JSON.parse(new TextDecoder().decode(sent.subarray(12))), settings)
    loaded.sent.length = 0
    loaded.receive(pageMessage(PAGE.READY, PAGE_B))
    const answer = loaded.sent.find(bytes => bytes[0] === PLUGIN.RESTORE)
    assert.equal(new DataView(answer.buffer).getUint32(4, true), PAGE_B, "a starting page gets the settings")
    assert.equal(new TextDecoder().decode(answer.subarray(12)), JSON.stringify(settings))
})

// the time the plugin takes: a block without a page (the ring), and with one page of each scope at its default settings
// polling every 6 blocks (a display frame): the mean and the worst block, and the poll (building the frame)
const timed = fn => {
    const start = process.hrtime.bigint()
    fn()
    return Number(process.hrtime.bigint() - start) / 1000
}
const blocks = (plugin, input, count) => {
    const times = []
    for (let block = 0; block < count; block++) {
        // the host's part: the input buffers
        for (let channel = 0; channel < 2; channel++) {
            for (let frame = 0; frame < BLOCK; frame++) {module.view.setFloat32(plugin.channels[channel] + frame * 4, input(channel, frame), true)}
        }
        times.push(timed(() => module.fn(plugin.plugin + 36)(plugin.plugin, plugin.process)))
    }
    return times
}
{
    const input = sine(1000)
    const idle = new Plugin(module, descriptors[0].id)
    idle.activate()
    idle.run(input)
    const base = blocks(idle, input, 2000)
    console.log(`  no page: ${(base.reduce((a, b) => a + b) / base.length).toFixed(1)} µs per block`)
    for (const scope of SCOPES) {
        const plugin = started(scope)
        if (scope === "spectrogram") {plugin.receive(config(PAGE_A, 1, {fftSize: 4096, rowCount: 300}))}
        if (scope === "waterfall") {plugin.receive(config(PAGE_A, 1, {ridges: 32, columns: 256}))}
        plugin.run(input)
        const times = [], polls = []
        for (let frame = 0; frame < 300; frame++) {
            times.push(...blocks(plugin, input, 6))
            polls.push(timed(() => plugin.receive(pageMessage(PAGE.POLL, PAGE_A))))
            plugin.sent.length = 0
        }
        const mean = values => values.reduce((a, b) => a + b) / values.length
        console.log(`  ${scope}: ${mean(times).toFixed(0)} µs per block (worst ${Math.max(...times.slice(60)).toFixed(0)}), `
            + `${mean(polls).toFixed(0)} µs per poll (worst ${Math.max(...polls.slice(10)).toFixed(0)}), ${framesBytes(plugin)} bytes a frame`)
    }
}
function framesBytes(plugin) {
    plugin.sent.length = 0
    plugin.receive(pageMessage(PAGE.POLL, PAGE_A))
    return plugin.sent[0]?.length ?? 0
}

console.log(`${checks} checks passed`)
