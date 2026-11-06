// Signalsmith Basics in node: a minimal CLAP host checks every plugin against Signalsmith's own WebCLAP build (clap
// ids, parameter ids, names, flags, defaults, its state bytes), that its states load, how many state bytes an edit
// changes, the audio (finite, latency) and the page traffic. Usage: node check.mjs <module.wasm>

import assert from "node:assert/strict"
import {readFileSync} from "node:fs"

const SAMPLE_RATE = 48000
const BLOCK = 128
const PAGE = {POLL: 1, BEGIN: 2, SET: 3, END: 4, HOVER: 7}
const VALTYPE = {i32: 0x7f, i64: 0x7e, f32: 0x7d, f64: 0x7c}
const IS_STEPPED = 1, IS_AUTOMATABLE = 32

// Signalsmith's build (WebCLAP/examples, signalsmith-basics/basics.wclap.tar.gz): [id, name, flags, min, max, default]
const SIGNALSMITH = {
    "uk.co.signalsmith.basics.chorus": {
        name: "[Basics] Chorus", latency: 0,
        params: [
            [1223226259, "mix", 32, 0, 1, 0.6],
            [1841279726, "depth", 32, 0, 1, 0.5],
            [2967225864, "detune", 32, 0, 1, 0.347826086957],
            [3972183190, "stereo", 32, 0, 1, 0.8]
        ],
        state: "bf6776657273696f6e00636d6978bf6576616c7565fb3fe3333333333333ff6764657074684d73bf6576616c7565fb402e000000000000ff66646574756e65bf6576616c7565fb4014000000000000ff6673746572656fbf6576616c7565fb3ff0000000000000ffff"
    },
    "uk.co.signalsmith.basics.crunch": {
        name: "[Basics] Crunch", latency: 32,
        params: [
            [2943184073, "drive", 32, 0, 1, 0.5],
            [481700, "fuzz", 32, 0, 1, 0],
            [4249490896, "tone", 32, 0, 1, 0.302186878728],
            [1100269863, "cut", 32, 0, 1, 0.25],
            [2537258908, "out", 32, 0, 1, 0.5]
        ],
        state: "bf6776657273696f6e00656472697665bf6576616c7565fb4010000000000000ff6466757a7abf6576616c7565fb0000000000000000ff66746f6e65487abf6576616c7565fb409f400000000000ff65637574487abf6576616c7565fb4049000000000000ff676f75744761696ebf6576616c7565fb3ff0000000000000ffff"
    },
    "uk.co.signalsmith.basics.freq-shifter": {
        name: "[Basics] Frequency Shifter", latency: 0,
        params: [
            [1223226259, "mix", 32, 0, 1, 1],
            [2301226280, "shift", 32, 0, 1, 0.724568837309],
            [3716977271, "reflect", 33, 0, 3, 0]
        ],
        state: "bf6776657273696f6e00636d6978bf6576616c7565fb3ff0000000000000ff677368696674487abf6576616c7565fb4049000000000000ff677265666c656374bf6576616c756500ffff"
    },
    "uk.co.signalsmith.basics.limiter": {
        name: "[Basics] Limiter", latency: 960,
        params: [
            [224108168, "pre-gain", 32, 0, 1, 0.5],
            [1951529716, "limit", 32, 0, 1, 0.89785630202],
            [274825181, "attack", 32, 0, 1, 0.73786407767],
            [1212264141, "hold", 32, 0, 1, 0],
            [1417304825, "release", 32, 0, 1, 0],
            [870994041, "smoothing", 33, 1, 2, 1],
            [2439531228, "link", 32, 0, 1, 0.5]
        ],
        state: "bf6776657273696f6e0469696e7075744761696ebf6576616c7565fb3ff0000000000000ff6b6f75747075744c696d6974bf6576616c7565fb3fe6a77deae8ab8aff6861747461636b4d73bf6576616c7565fb4034000000000000ff66686f6c644d73bf6576616c7565fb0000000000000000ff6972656c656173654d73bf6576616c7565fb0000000000000000ff6f736d6f6f7468696e67537461676573bf6576616c756501ff6c6c696e6b4368616e6e656c73bf6576616c7565fb3fe0000000000000ffff"
    },
    "uk.co.signalsmith.basics.reverb": {
        name: "[Basics] Reverb", latency: 0,
        params: [
            [1015595465, "dry", 32, 0, 1, 0.5],
            [3538153984, "wet", 32, 0, 1, 0.3],
            [2974242839, "room", 32, 0, 1, 0.393258426966],
            [1506047092, "decay", 32, 0, 1, 0.324476179328],
            [867170651, "early", 32, 0, 1, 0.692307692308],
            [2967225864, "detune", 32, 0, 1, 0.272727272727],
            [2342541086, "low cut", 32, 0, 1, 0.5],
            [1970510480, "low damp", 32, 0, 1, 0.32],
            [1157094815, "high cut", 32, 0, 1, 0.837563451777],
            [2716295038, "high damp", 32, 0, 1, 0.615384615385]
        ],
        state: "bf6776657273696f6e0563647279bf6576616c7565fb3ff0000000000000ff63776574bf6576616c7565fb3fe0000000000000ff66726f6f6d4d73bf6576616c7565fb4054000000000000ff6472743230bf6576616c7565fb3ff0000000000000ff656561726c79bf6576616c7565fb3ff8000000000000ff66646574756e65bf6576616c7565fb4000000000000000ff686c6f77437574487abf6576616c7565fb4054000000000000ff6b6c6f7744616d7052617465bf6576616c7565fb3ff8000000000000ff6968696768437574487abf6576616c7565fb40c7700000000000ff6c6869676844616d7052617465bf6576616c7565fb4004000000000000ffff"
    },
    "uk.co.signalsmith.basics.analyser": {
        name: "[Basics] Analyser", latency: 0,
        params: [
            [2639851652, "res.", 32, 0, 1, 0.5]
        ],
        state: "bf6776657273696f6e006e6261726b5265736f6c7574696f6ebf6576616c7565fb4024000000000000ffff"
    }
}

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
        const strings = ["basics check", "open-webclap", "", "1"].map(text => m.string(text))
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
const close = (actual, expected, what) => assert.ok(Math.abs(actual - expected) < 1e-9, `${what}: ${actual} != ${expected}`)

const module = await Module.load(process.argv[2])
const descriptors = module.descriptors()

check("seven plugins, the six of Signalsmith's build among them, each with the device panel", () => {
    assert.equal(descriptors.length, 7)
    for (const id of Object.keys(SIGNALSMITH)) {
        const descriptor = descriptors.find(d => d.id === id)
        assert.ok(descriptor, id)
        assert.equal(descriptor.name, SIGNALSMITH[id].name)
    }
    for (const descriptor of descriptors) {assert.ok(descriptor.features.includes("opendaw:device-panel"), descriptor.id)}
})

for (const [id, expected] of Object.entries(SIGNALSMITH)) {
    const short = id.split(".").pop()
    check(`${short}: parameter ids, names, ranges, flags and defaults as Signalsmith's`, () => {
        const plugin = new Plugin(module, id)
        const infos = plugin.paramInfos()
        assert.equal(infos.length, expected.params.length)
        expected.params.forEach(([pid, name, flags, min, max, defaultValue], index) => {
            const info = infos[index]
            assert.equal(info.id, pid, `${name} id`)
            assert.equal(info.name, name)
            assert.equal(info.flags & (IS_STEPPED | IS_AUTOMATABLE), flags, `${name} flags`)
            assert.equal(info.min, min)
            assert.equal(info.max, max)
            assert.ok(Math.abs(info.default - defaultValue) < 1e-9, `${name} default ${info.default} != ${defaultValue}`)
        })
    })
    check(`${short}: the default state is Signalsmith's, byte for byte`, () => {
        const plugin = new Plugin(module, id)
        assert.equal(plugin.save().toString("hex"), expected.state)
    })
    check(`${short}: a Signalsmith state with other values loads them, and saves them back the same`, () => {
        // every float64 value one fifth up its range (in knob positions), every step to its last
        const plugin = new Plugin(module, id)
        const infos = plugin.paramInfos()
        const target = infos.map(info => info.flags & IS_STEPPED ? info.max : Math.min(1, info.default + 0.2))
        const source = new Plugin(module, id)
        source.activate()
        source.run(() => 0, infos.map((info, index) => [0, info.id, target[index]]))
        const bytes = source.save()
        assert.equal(bytes.length, expected.state.length / 2, "same layout")
        assert.ok(plugin.load(bytes))
        infos.forEach((info, index) => close(plugin.value(info.id), target[index], info.name))
        assert.equal(plugin.save().toString("hex"), bytes.toString("hex"))
    })
}

check("dynamics: thirteen parameters, a sidechain input, its state in groups", () => {
    const plugin = new Plugin(module, "uk.co.signalsmith.basics.dynamics")
    const infos = plugin.paramInfos()
    assert.equal(infos.length, 13)
    assert.equal(new Set(infos.map(info => info.id)).size, 13, "unique ids")
    const m = module
    assert.equal(m.fn(plugin.audioPorts)(plugin.plugin, 1), 2, "main and sidechain in")
    assert.equal(m.fn(plugin.audioPorts)(plugin.plugin, 0), 1)
    const text = plugin.save().toString("latin1")
    for (const group of ["compressor", "expander", "gate", "timing"]) {assert.ok(text.includes(group), group)}
    assert.equal(plugin.text(infos[1].id, 1), "inf:1")
    assert.equal(plugin.text(infos[1].id, 0.5), "2.0:1")
})

check("dynamics: a silent sidechain (openDAW's when none is connected) leaves the detector on the main input", () => {
    const plugin = new Plugin(module, "uk.co.signalsmith.basics.dynamics")
    plugin.activate()
    let energy = 0
    for (let block = 0; block < 200; block++) {
        const [left] = plugin.run((channel, frame) => channel < 2 ? 0.5 * Math.sin((block * BLOCK + frame) * 0.06) : 0)
        if (block >= 100) {for (const sample of left) {energy += sample * sample}}
    }
    assert.ok(energy > 100, `passes (${energy})`)
    plugin.receive(new Uint8Array([PAGE.POLL]))
    const status = new DataView(plugin.sent.at(-1).buffer)
    const readings = 8 + 13 * 8
    assert.ok(status.getFloat32(readings + 12, true) > -12, "detector hears the input")
    assert.ok(status.getFloat32(readings + 16, true) < 0.9, "compresses above -20 dB")
})

check("an edit changes at most 8 state bytes, a step 1", () => {
    for (const {id} of descriptors) {
        const plugin = new Plugin(module, id)
        plugin.activate()
        const infos = plugin.paramInfos()
        for (const info of infos) {
            const before = plugin.save()
            const value = info.flags & IS_STEPPED ? info.max : 0.123
            plugin.run(() => 0, [[0, info.id, value]])
            const after = plugin.save()
            assert.equal(after.length, before.length, `${id} ${info.name}: same length`)
            let changed = 0
            for (let index = 0; index < after.length; index++) {if (after[index] !== before[index]) {changed++}}
            assert.ok(changed <= 8, `${id} ${info.name}: ${changed} bytes`)
            plugin.run(() => 0, [[0, info.id, info.default]])
        }
    }
})

check("texts and value entry in the units the editor shows", () => {
    const crunch = new Plugin(module, "uk.co.signalsmith.basics.crunch")
    const [drive, fuzz, tone] = crunch.paramInfos()
    assert.equal(crunch.text(drive.id, drive.default), "12.0dB")
    assert.equal(crunch.text(tone.id, 1), "20.0kHz")
    close(crunch.fromText(fuzz.id, "50%"), 0.5, "50%")
    assert.equal(crunch.text(tone.id, crunch.fromText(tone.id, "4k")), "4.00kHz")
    const shifter = new Plugin(module, "uk.co.signalsmith.basics.freq-shifter")
    const [, shift, reflect] = shifter.paramInfos()
    assert.equal(shifter.text(shift.id, 0.5), "0.0Hz")
    assert.equal(shifter.text(shift.id, 1), "1000Hz")
    assert.equal(shifter.fromText(reflect.id, "duplicate above 0Hz"), 2)
})

check("audio: finite for every effect, a sine passes, the latency is reported and follows the attack", () => {
    for (const {id} of descriptors) {
        const plugin = new Plugin(module, id)
        plugin.activate()
        let energy = 0
        for (let block = 0; block < 200; block++) {
            const [left, right] = plugin.run((channel, frame) => 0.5 * Math.sin(2 * Math.PI * 440 * (block * BLOCK + frame) / SAMPLE_RATE))
            for (let frame = 0; frame < BLOCK; frame++) {
                assert.ok(Number.isFinite(left[frame]) && Number.isFinite(right[frame]), `${id} finite`)
                energy += left[frame] * left[frame]
            }
        }
        assert.ok(energy > 1, `${id} passes signal (${energy})`)
    }
    const limiter = new Plugin(module, "uk.co.signalsmith.basics.limiter")
    limiter.activate()
    assert.equal(module.fn(limiter.latency)(limiter.plugin), SIGNALSMITH["uk.co.signalsmith.basics.limiter"].latency)
    const attack = limiter.paramInfos().find(info => info.name === "attack")
    limiter.run(() => 0, [[0, attack.id, 0]])
    assert.equal(module.fn(limiter.latency)(limiter.plugin), 48, "1 ms")
    assert.equal(limiter.latencyChanges, 1, "host told")
    const crunch = new Plugin(module, "uk.co.signalsmith.basics.crunch")
    crunch.activate()
    assert.equal(module.fn(crunch.latency)(crunch.plugin), 32)
})

check("page: a poll answers values and readings, a drag is one gesture marked dirty at its end, hover", () => {
    const limiter = new Plugin(module, "uk.co.signalsmith.basics.limiter")
    limiter.activate()
    const infos = limiter.paramInfos()
    for (let block = 0; block < 40; block++) {limiter.run((channel, frame) => Math.sin(frame) * 2)}
    assert.ok(limiter.receive(new Uint8Array([PAGE.POLL])))
    const status = new DataView(limiter.sent.at(-1).buffer)
    assert.equal(status.getUint8(0), 1)
    assert.equal(status.getUint8(1), infos.length)
    assert.equal(status.byteLength, 8 + infos.length * 8 + 20, "no spectrum")
    close(status.getFloat64(8 + 8, true), infos[1].default, "limit value")
    const readings = 8 + infos.length * 8
    assert.ok(status.getFloat32(readings, true) > 1.5, "input peak")
    assert.ok(status.getFloat32(readings + 16, true) < 0.5, "limiting")
    const limit = infos[1].id
    limiter.pageParam(PAGE.BEGIN, limit)
    for (const value of [0.5, 0.4, 0.3]) {limiter.pageParam(PAGE.SET, limit, value)}
    assert.equal(limiter.dirty, 0, "no undo step while dragging")
    limiter.pageParam(PAGE.END, limit)
    assert.equal(limiter.dirty, 1)
    limiter.run(() => 0)
    assert.deepEqual(limiter.out.map(event => event.type), [7, 5, 5, 5, 8], "begin, values, end")
    close(limiter.value(limit), 0.3, "page value")
    const hover = new DataView(new ArrayBuffer(8))
    hover.setUint8(0, PAGE.HOVER)
    hover.setUint32(4, limit, true)
    limiter.receive(new Uint8Array(hover.buffer))
    assert.equal(limiter.hovered.at(-1), limit)

    const analyser = new Plugin(module, "uk.co.signalsmith.basics.analyser")
    analyser.activate()
    for (let block = 0; block < 40; block++) {analyser.run((channel, frame) => Math.sin((block * BLOCK + frame) * 0.05))}
    analyser.receive(new Uint8Array([PAGE.POLL]))
    const spectrum = new DataView(analyser.sent.at(-1).buffer)
    const bands = spectrum.getUint16(2, true)
    assert.ok(bands > 100 && bands < 1024, `bands ${bands}`)
    assert.equal(spectrum.byteLength, 8 + 8 + 20 + bands * 12)
    const chorus = new Plugin(module, "uk.co.signalsmith.basics.chorus")
    chorus.activate()
    for (let block = 0; block < 400; block++) {chorus.run(() => 0)}
    chorus.receive(new Uint8Array([PAGE.POLL]))
    const phase = new DataView(chorus.sent.at(-1).buffer).getFloat32(8 + 4 * 8 + 8, true)
    assert.ok(phase > 0 && phase < 1, `phase ${phase}`)
})

check("device panel and window: the card's width at 176 px as declared, the window at its aspect ratio", () => {
    for (const {id, features} of descriptors) {
        const plugin = new Plugin(module, id)
        const m = module
        const width = m.malloc(4)
        assert.ok(m.fn(plugin.panel + 4)(plugin.plugin, 176, width))
        const panel = m.u32(width)
        assert.ok(panel >= 246 && panel <= 382, `${id} ${panel}`)
        assert.ok(features.includes(`opendaw:device-panel-width:${panel}`), `${id} ${panel}`)
        const uri = m.malloc(64)
        m.fn(plugin.panel)(plugin.plugin, uri, 64)
        assert.ok(m.cstr(uri).startsWith("/index.html#"), m.cstr(uri))
        const size = m.malloc(8)
        m.fn(plugin.gui + 20)(plugin.plugin, size, size + 4)
        assert.equal(m.u32(size), panel * 2)
        assert.equal(m.u32(size + 4), 352)
    }
})

console.log(`signalsmith-basics: all ${checks} checks passed`)
