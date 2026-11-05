// Pearls in node: a minimal CLAP host checks the descriptor and parameters, the latency it reports against an impulse
// through the dry path at each quality and host rate, how clean the resampling is, that grains and freeze sound,
// the state and the page traffic. Usage: node check.mjs <module.wasm>

import assert from "node:assert/strict"
import {readFileSync} from "node:fs"

const SAMPLE_RATE = 48000
const BLOCK = 128
const PAGE = {POLL: 1, BEGIN: 2, SET: 3, END: 4, HOVER: 7}
const VALTYPE = {i32: 0x7f, i64: 0x7e, f32: 0x7d, f64: 0x7c}
const IS_STEPPED = 1

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
        for (const entry of WebAssembly.Module.imports(module)) {
            (imports[entry.module] ??= {})[entry.name] = () => {throw new Error(`${entry.module}.${entry.name} called`)}
        }
        const instance = await WebAssembly.instantiate(module, imports)
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
        const strings = ["pearls check", "open-webclap", "", "1"].map(text => m.string(text))
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

    activate(rate = SAMPLE_RATE) {
        const m = this.module
        assert.ok(m.fn(this.plugin + 16)(this.plugin, rate, 1, BLOCK), "activate")
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
        // a transport while one is set: the beat position at this block, the tempo, playing
        if (this.transport) {
            const t = this.transportPointer ??= m.malloc(128)
            m.bytes.fill(0, t, t + 128)
            m.view.setUint32(t, 128, true)
            m.view.setUint16(t + 10, 9, true)
            // clap_event_transport: the header, flags at 16, song_pos_beats at 24, tempo at 40
            m.view.setUint32(t + 16, 1 | 2 | (this.transport.playing ? 16 : 0), true)
            m.view.setBigInt64(t + 24, BigInt(Math.round(this.transport.beat * 2 ** 31)), true)
            m.view.setFloat64(t + 40, this.transport.tempo, true)
            this.transport.beat += BLOCK * this.transport.tempo / 60 / this.transport.rate
        }
        m.view.setUint32(p + 12, this.transport ? this.transportPointer : 0, true)
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

    pageParam(type, id, value) {
        const view = new DataView(new ArrayBuffer(type === PAGE.SET ? 16 : 8))
        view.setUint8(0, type)
        view.setUint32(4, id, true)
        if (type === PAGE.SET) {view.setFloat64(8, value, true)}
        assert.ok(this.receive(new Uint8Array(view.buffer)))
    }
}

let checks = 0
const check = (name, fn) => {
    fn()
    checks++
    console.log(`ok ${name}`)
}

const fnv1a = text => {
    let hash = 0x811c9dc5
    for (const letter of new TextEncoder().encode(text)) {hash = Math.imul(hash ^ letter, 16777619) >>> 0}
    return hash & 0x7fffffff
}
const ID = Object.fromEntries(["time", "density", "pitch", "size", "shape", "timeRandom", "pitchRandom", "sizeRandom",
    "shapeRandom", "feedback", "dryWet", "reverb", "input", "output", "quality", "record", "freeze",
    "quantize", "sync"].map(key => [key, fnv1a(key)]))
const QUALITIES = ["dirty tape", "clean tape", "lo-fi digital", "hi-fi digital"]

const module = await Module.load(process.argv[2])
const PLUGIN = "studio.opendaw.pearls"
const latencyOf = plugin => plugin.module.fn(plugin.latency)(plugin.plugin)

// blocks of output for a signal(frame) on both channels, with events in the first block
const render = (plugin, blocks, signal, events = []) => {
    const left = [], right = []
    for (let block = 0; block < blocks; block++) {
        const [l, r] = plugin.run((_channel, frame) => signal(block * BLOCK + frame), block === 0 ? events : [])
        left.push(...l)
        right.push(...r)
    }
    return [left, right]
}
const rms = samples => Math.sqrt(samples.reduce((sum, x) => sum + x * x, 0) / samples.length)
const finite = samples => samples.every(Number.isFinite)

check("one plugin with the device panel, its parameters with unique ids and their ranges", () => {
    const descriptors = module.descriptors()
    assert.equal(descriptors.length, 1)
    assert.equal(descriptors[0].id, PLUGIN)
    assert.ok(descriptors[0].features.includes("opendaw:device-panel"))
    assert.ok(descriptors[0].features.includes("opendaw:device-panel-width:636"))
    assert.ok(descriptors[0].features.includes("audio-effect"))
    const plugin = new Plugin(module, PLUGIN)
    const infos = plugin.paramInfos()
    assert.equal(infos.length, Object.keys(ID).length)
    assert.deepEqual(infos.map(info => info.id), Object.values(ID))
    // openDAW reads a hovered id as signed: every id stays below 2^31
    for (const info of infos) {assert.ok(info.id < 2 ** 31, `${info.name}: ${info.id}`)}
    const byId = new Map(infos.map(info => [info.id, info]))
    assert.equal(byId.get(ID.pitch).min, -24)
    assert.equal(byId.get(ID.pitch).max, 24)
    assert.equal(byId.get(ID.quality).max, 3)
    assert.ok(byId.get(ID.quality).flags & IS_STEPPED)
    assert.equal(plugin.text(ID.quality, 1), "Clean tape")
    assert.equal(plugin.text(ID.pitch, 7), "+7.00 st")
    assert.equal(plugin.fromText(ID.quality, "lo-fi"), 2)
    assert.equal(plugin.fromText(ID.dryWet, "40%"), 0.4)
})

// an impulse through the dry path only: it comes out at the latency, at every quality and host rate
for (const rate of [48000, 44100, 96000]) {
    check(`at ${rate / 1000} kHz the dry impulse comes out at the reported latency, at each quality`, () => {
        for (let quality = 0; quality < 4; quality++) {
            const plugin = new Plugin(module, PLUGIN)
            plugin.run = plugin.run.bind(plugin)
            plugin.activate(rate)
            render(plugin, 2, () => 0, [[0, ID.dryWet, 0], [0, ID.reverb, 0], [0, ID.quality, quality]])
            // the restart fades out and in over 10 ms
            render(plugin, Math.ceil(rate * 0.05 / BLOCK), () => 0)
            const latency = latencyOf(plugin)
            const at = 100
            const [left] = render(plugin, Math.ceil((at + latency + 400) / BLOCK), frame => frame === at ? 0.5 : 0)
            assert.ok(finite(left))
            let peak = 0
            left.forEach((x, index) => {if (Math.abs(x) > Math.abs(left[peak])) {peak = index}})
            // the dirty tape's wow and flutter move it by up to 73 samples at 24 kHz around the average it reports
            const slack = quality === 0 ? Math.ceil(73 * rate / 24000) : 1
            assert.ok(Math.abs(peak - (at + latency)) <= slack,
                `${QUALITIES[quality]}: peak at ${peak - at}, latency ${latency}`)
        }
    })
}

check("the resampling keeps a sine clean (hi-fi at 44.1 kHz: 48 kHz inside)", () => {
    const plugin = new Plugin(module, PLUGIN)
    plugin.activate(44100)
    render(plugin, 2, () => 0, [[0, ID.dryWet, 0], [0, ID.reverb, 0]])
    const latency = latencyOf(plugin)
    const sine = frame => 0.25 * Math.sin(2 * Math.PI * 1000 * frame / 44100)
    const [left] = render(plugin, 200, sine)
    // the tail of it against the input delayed by the latency, at the best gain
    const from = 8000, output = left.slice(from), input = output.map((_, index) => sine(from + index - latency))
    const gain = output.reduce((sum, x, index) => sum + x * input[index], 0) / input.reduce((sum, x) => sum + x * x, 0)
    const error = rms(output.map((x, index) => x - gain * input[index])) / rms(input.map(x => x * gain))
    assert.ok(gain > 0.6 && gain < 0.9, `gain ${gain}`)
    assert.ok(20 * Math.log10(error) < -60, `error ${(20 * Math.log10(error)).toFixed(1)} dB`)
})

check("grains sound from the input, and go on frozen when it stops", () => {
    const plugin = new Plugin(module, PLUGIN)
    plugin.activate()
    let seed = 1
    const noise = () => {
        seed = (seed * 16807) % 2147483647
        return (seed / 2147483647 - 0.5) * 0.5
    }
    const [wet] = render(plugin, 400, noise, [[0, ID.dryWet, 1], [0, ID.reverb, 0], [0, ID.density, 0.85]])
    assert.ok(finite(wet))
    assert.ok(rms(wet.slice(-24000)) > 0.01, `grains ${rms(wet.slice(-24000))}`)
    render(plugin, 1, () => 0, [[0, ID.freeze, 1]])
    const [frozen] = render(plugin, 400, () => 0)
    assert.ok(rms(frozen.slice(-24000)) > 0.01, `frozen ${rms(frozen.slice(-24000))}`)
    render(plugin, 1, () => 0, [[0, ID.freeze, 0]])
    const [after] = render(plugin, 1200, () => 0)
    assert.ok(rms(after.slice(-24000)) < 1e-3, `silent again ${rms(after.slice(-24000))}`)
})

check("every quality, delay mode (size at its end) and mono sound, finite, and announce latency changes", () => {
    const plugin = new Plugin(module, PLUGIN)
    plugin.activate()
    const tone = frame => 0.3 * Math.sin(frame * 0.03) * (frame % 9600 < 4800 ? 1 : 0.2)
    // Time near the write head: a restart empties the buffer, grains further back read silence until it fills
    render(plugin, 2, tone, [[0, ID.dryWet, 1], [0, ID.density, 0.85], [0, ID.time, 0.02]])
    const before = plugin.latencyChanges
    for (const [events, what] of [
        [[[0, ID.quality, 0]], "dirty tape"], [[[0, ID.quality, 1]], "clean tape"], [[[0, ID.quality, 2]], "lo-fi"],
        [[[0, ID.quality, 3], [0, ID.record, 1]], "mono"], [[[0, ID.record, 0], [0, ID.size, 1]], "delay"]
    ]) {
        const [left, right] = render(plugin, 400, tone, events)
        assert.ok(finite(left) && finite(right), what)
        assert.ok(rms(left.slice(-24000)) > 0.005, `${what}: ${rms(left.slice(-24000))}`)
        assert.ok(Math.max(...left.map(Math.abs)) < 1.5, `${what} bounded`)
    }
    assert.ok(plugin.latencyChanges > before, "latency changes announced")
})

// the loudness in windows of 10 ms and where it rises: the grains' onsets
const onsets = samples => {
    const window = 480, levels = []
    for (let at = 0; at + window <= samples.length; at += window) {levels.push(rms(samples.slice(at, at + window)))}
    const peak = Math.max(...levels), found = []
    levels.forEach((level, index) => {
        if (index > 0 && level > peak * 0.5 && levels[index - 1] <= peak * 0.5) {found.push(index * window)}
    })
    return found
}

check("Sync: the host's quarter notes clock the grains while the transport plays, Density divides them", () => {
    const plugin = new Plugin(module, PLUGIN)
    plugin.activate()
    plugin.transport = {beat: 0, tempo: 120, playing: true, rate: SAMPLE_RATE}
    const tone = frame => 0.4 * Math.sin(frame * 0.05)
    const events = [[0, ID.dryWet, 1], [0, ID.reverb, 0], [0, ID.feedback, 0], [0, ID.time, 0.02], [0, ID.size, 0.3],
        [0, ID.density, 0], [0, ID.sync, 4]]
    render(plugin, 100, tone, events)
    const [quarters] = render(plugin, 1500, tone)
    const gaps = onsets(quarters).slice(1).map((at, index, all) => at - (index === 0 ? onsets(quarters)[0] : all[index - 1]))
    assert.ok(gaps.length >= 6, `onsets ${onsets(quarters)}`)
    for (const gap of gaps) {assert.ok(Math.abs(gap - 24000) <= 960, `a quarter at 120 bpm: ${gaps}`)}
    // left of noon the density divides the clock: every second quarter, a half note
    render(plugin, 1, tone, [[0, ID.density, 0.12]])
    render(plugin, 400, tone)
    const [halves] = render(plugin, 1500, tone)
    const found = onsets(halves)
    for (let index = 1; index < found.length; index++) {
        assert.ok(Math.abs(found[index] - found[index - 1] - 48000) <= 960, `halves: ${found}`)
    }
    assert.ok(found.length >= 3, `halves: ${found}`)
})

check("Sync off, or the transport stopped: the grains run free", () => {
    for (const setup of [{sync: 0, playing: true}, {sync: 4, playing: false}]) {
        const plugin = new Plugin(module, PLUGIN)
        plugin.activate()
        plugin.transport = {beat: 0, tempo: 120, playing: setup.playing, rate: SAMPLE_RATE}
        const tone = frame => 0.4 * Math.sin(frame * 0.05)
        render(plugin, 100, tone, [[0, ID.dryWet, 1], [0, ID.reverb, 0], [0, ID.time, 0.02], [0, ID.size, 0.3],
            [0, ID.density, 0.2], [0, ID.sync, setup.sync]])
        const [free] = render(plugin, 750, tone)
        const found = onsets(free)
        const gaps = found.slice(1).map((at, index) => at - found[index])
        assert.ok(gaps.length >= 4 && gaps.every(gap => Math.abs(gap - 24000) > 1500),
            `${JSON.stringify(setup)}: ${found}`)
    }
})

check("the state keeps every value, by id; a foreign state is refused", () => {
    const source = new Plugin(module, PLUGIN)
    source.activate()
    const values = {time: 0.8, density: 0.2, pitch: -7, size: 0.3, shape: 0.9, timeRandom: -0.5, pitchRandom: 0.25,
        sizeRandom: 0.1, shapeRandom: -1, feedback: 0.6, dryWet: 0.4, reverb: 0.7, input: 6, output: -3, quality: 1,
        record: 1, freeze: 1, quantize: 1, sync: 3}
    render(source, 1, () => 0, Object.entries(values).map(([key, value]) => [0, ID[key], value]))
    const bytes = source.save()
    assert.equal(bytes.subarray(0, 4).toString(), "PRL1")
    assert.equal(bytes.length, 8 + 19 * 12)
    const target = new Plugin(module, PLUGIN)
    assert.ok(target.load(bytes))
    for (const [key, value] of Object.entries(values)) {
        assert.equal(target.value(ID[key]), value, key)
    }
    assert.ok(!target.load(Buffer.from("CBOR....")))
})

check("page: the status (values and peaks), a drag as one undo step, the hovered parameter", () => {
    const plugin = new Plugin(module, PLUGIN)
    plugin.activate()
    render(plugin, 4, frame => 0.5 * Math.sin(frame * 0.1))
    assert.ok(plugin.receive(new Uint8Array([PAGE.POLL])))
    const status = plugin.sent.pop()
    const count = Object.keys(ID).length
    assert.equal(status[0], 1)
    assert.equal(status[1], count)
    assert.equal(status.length, 8 + count * 8 + 8)
    const view = new DataView(status.buffer, status.byteOffset)
    assert.equal(view.getFloat64(8 + 3 * 8, true), 0.6, "size")
    assert.ok(view.getFloat32(8 + count * 8, true) > 0.4, "input peak")
    plugin.pageParam(PAGE.BEGIN, ID.time)
    plugin.pageParam(PAGE.SET, ID.time, 0.4)
    plugin.pageParam(PAGE.SET, ID.time, 0.45)
    assert.equal(plugin.dirty, 0)
    plugin.pageParam(PAGE.END, ID.time)
    assert.equal(plugin.dirty, 1)
    assert.equal(plugin.value(ID.time), 0.45)
    render(plugin, 1, () => 0)
    assert.deepEqual(plugin.out.map(event => event.type), [7, 5, 5, 8])
    plugin.pageParam(PAGE.HOVER, ID.pitch)
    plugin.pageParam(PAGE.HOVER, 0xffffffff)
    assert.deepEqual(plugin.hovered, [ID.pitch, 0xffffffff])
})

console.log(`${checks} checks passed`)
