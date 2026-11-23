// Elements (faust-elements) in node: a minimal CLAP host (after ports/oxo-synth2/check.mjs) plays notes and checks
// the audio (a note sounds at the right pitch through the 32 kHz -> host-rate resampler, release and sleep work,
// a pitch-bend note expression transposes, the resonator model switches), the 21 parameters and their menu texts,
// the 96-byte state round-trip, the page protocol (hello/describe/snapshot, poll, set, patch, panic, note on/off),
// then measures the DSP (8-note chord). Usage: node check.mjs <module.wasm>

import assert from "node:assert/strict"
import {readFileSync} from "node:fs"

const SAMPLE_RATE = 48000
const BLOCK = 128
const EVENT = {NOTE_ON: 0, NOTE_OFF: 1, NOTE_CHOKE: 2, NOTE_EXPRESSION: 4, PARAM_VALUE: 5, MIDI: 10, GESTURE_BEGIN: 7, GESTURE_END: 8}
const PAGE = {HELLO: 1, POLL: 2, BEGIN: 3, SET: 4, END: 5, HOVER: 6, PATCH: 8, DEFAULTS: 9, PANIC: 10, NOTE_ON: 11, NOTE_OFF: 12}
const PLUGIN = {DESCRIBE: 0x81, SNAPSHOT: 0x82, VALUES: 0x83, ACTIVITY: 0x84}
const STATUS = {CONTINUE: 1, SLEEP: 4}
const PARAMS = 21
const STATE_SIZE = 12 + PARAMS * 4
const VALTYPE = {i32: 0x7f, i64: 0x7e, f32: 0x7d, f64: 0x7c}

const trampoline = (params, results, fn) => {
    const name = text => [text.length, ...[...text].map(letter => letter.charCodeAt(0))]
    const section = (id, bytes) => [id, bytes.length, ...bytes]
    const type = [1, 0x60, params.length, ...params.map(kind => VALTYPE[kind]), results.length, ...results.map(kind => VALTYPE[kind])]
    const bytes = new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        ...section(1, type), ...section(2, [1, ...name("e"), ...name("f"), 0x00, 0x00]), ...section(7, [1, ...name("f"), 0x00, 0x00])])
    return new WebAssembly.Instance(new WebAssembly.Module(bytes), {e: {f: fn}}).exports.f
}

class Host {
    static async load(path, sampleRate = SAMPLE_RATE) {
        const module = await WebAssembly.compile(readFileSync(path))
        for (const entry of WebAssembly.Module.imports(module))
            assert.equal(entry.module, "wasi_snapshot_preview1", `import ${entry.module}.${entry.name} is not WASI`)
        const imports = {}
        for (const entry of WebAssembly.Module.imports(module))
            (imports[entry.module] ??= {})[entry.name] = () => 0
        const instance = await WebAssembly.instantiate(module, imports)
        instance.exports._initialize?.()
        return new Host(instance.exports, sampleRate)
    }

    constructor(exports, sampleRate) {
        this.sampleRate = sampleRate
        this.memory = exports.memory
        this.table = exports.__indirect_function_table
        this.malloc = exports.malloc
        this.out = []
        this.sent = []
        this.flushes = 0
        this.dirty = 0
        this.hovered = -2
        this.position = 0
        const entry = exports.clap_entry.value
        assert.ok(this.fn(entry + 12)(this.string("/")), "entry init")
        const factory = this.fn(entry + 20)(this.string("clap.plugin-factory"))
        const descriptor = this.fn(factory + 4)(factory, 0)
        this.descriptor = descriptor
        this.id = this.cstr(this.u32(descriptor + 12))
        assert.equal(this.id, "org.faust.elements")
        this.name = this.cstr(this.u32(descriptor + 16))
        this.version = this.cstr(this.u32(descriptor + 36))
        this.createHost()
        this.plugin = this.fn(factory + 8)(factory, this.host, this.string(this.id))
        assert.ok(this.plugin, "create")
        assert.ok(this.fn(this.plugin + 8)(this.plugin), "init")
        const extension = name => this.fn(this.plugin + 40)(this.plugin, this.string(name))
        this.params = extension("clap.params")
        this.state = extension("clap.state")
        this.tail = extension("clap.tail")
        this.webview = extension("clap.webview/3")
        this.gui = extension("clap.gui")
        assert.ok(this.params && this.state && this.tail && this.webview && this.gui, "extensions")
        this.createProcess()
        assert.ok(this.fn(this.plugin + 16)(this.plugin, sampleRate, 1, BLOCK), "activate")
        assert.ok(this.fn(this.plugin + 24)(this.plugin), "start_processing")
        assert.ok(this.fn(this.gui + 8)(this.plugin, this.string("webview"), 0), "gui create")
    }

    get view() {return new DataView(this.memory.buffer)}
    get bytes() {return new Uint8Array(this.memory.buffer)}
    u32(pointer) {return this.view.getUint32(pointer, true)}
    fn(pointer) {return this.table.get(this.u32(pointer))}
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

    createHost() {
        const host = this.malloc(48)
        const extensions = new Map()
        const extension = (name, size) => {
            const pointer = this.malloc(size)
            extensions.set(name, pointer)
            return pointer
        }
        const strings = ["elements check", "open-webclap", "", "1"].map(text => this.string(text))
        this.view.setUint32(host, 1, true)
        strings.forEach((pointer, index) => this.view.setUint32(host + 16 + index * 4, pointer, true))
        this.install(host + 32, ["i32", "i32"], ["i32"], (_host, id) => extensions.get(this.cstr(id)) ?? 0)
        this.install(host + 36, ["i32"], [], () => {})
        this.install(host + 40, ["i32"], [], () => {})
        this.install(host + 44, ["i32"], [], () => {})
        const params = extension("clap.params", 12)
        this.install(params, ["i32", "i32"], [], () => {})
        this.install(params + 4, ["i32", "i32", "i32"], [], () => {})
        this.install(params + 8, ["i32"], [], () => this.flushes++)
        const state = extension("clap.state", 4)
        this.install(state, ["i32"], [], () => this.dirty++)
        const webview = extension("clap.webview/3", 4)
        this.install(webview, ["i32", "i32", "i32"], ["i32"], (_host, pointer, size) => {
            this.sent.push(this.bytes.slice(pointer, pointer + size))
            return 1
        })
        const hovered = extension("clap.param-hovered/1", 4)
        this.install(hovered, ["i32", "i32"], [], (_host, id) => { this.hovered = id })
        this.host = host
    }

    createProcess() {
        this.process = this.malloc(40)
        this.slots = this.malloc(48 * 512)
        this.inEvents = this.malloc(12)
        this.install(this.inEvents + 4, ["i32"], ["i32"], () => this.count)
        this.install(this.inEvents + 8, ["i32", "i32"], ["i32"], (_list, index) => index < this.count ? this.slots + index * 48 : 0)
        this.outEvents = this.malloc(8)
        this.install(this.outEvents + 4, ["i32", "i32"], ["i32"], (_list, pointer) => (this.read(pointer), 1))
        this.ostream = this.malloc(8)
        this.install(this.ostream + 4, ["i32", "i32", "i64"], ["i64"], (_stream, pointer, size) => {
            this.written.push(...this.bytes.slice(pointer, pointer + Number(size)))
            return size
        })
        this.istream = this.malloc(8)
        this.install(this.istream + 4, ["i32", "i32", "i64"], ["i64"], (_stream, pointer, size) => {
            const count = Math.min(Number(size), this.source.length - this.cursor)
            this.bytes.set(this.source.subarray(this.cursor, this.cursor + count), pointer)
            this.cursor += count
            return BigInt(count)
        })
        this.channels = [this.malloc(4 * 65536), this.malloc(4 * 65536)]
        this.channelPointers = this.malloc(8)
        this.view.setUint32(this.channelPointers, this.channels[0], true)
        this.view.setUint32(this.channelPointers + 4, this.channels[1], true)
        this.audioOut = this.malloc(24)
        this.bytes.fill(0, this.audioOut, this.audioOut + 24)
        this.view.setUint32(this.audioOut, this.channelPointers, true)
        this.view.setUint32(this.audioOut + 8, 2, true)
        this.scratch = this.malloc(8192)
        this.count = 0
    }

    read(pointer) {
        const view = this.view
        const type = view.getUint16(pointer + 10, true)
        if (type === EVENT.PARAM_VALUE)
            this.out.push({type, param: view.getUint32(pointer + 16, true), value: view.getFloat64(pointer + 40, true)})
        else if (type === EVENT.GESTURE_BEGIN || type === EVENT.GESTURE_END)
            this.out.push({type, param: view.getUint32(pointer + 16, true)})
    }

    write(events) {
        const view = this.view
        events.forEach((event, index) => {
            const pointer = this.slots + index * 48
            this.bytes.fill(0, pointer, pointer + 48)
            view.setUint32(pointer + 4, event.time, true)
            view.setUint16(pointer + 10, event.type, true)
            if (event.type === EVENT.PARAM_VALUE) {
                view.setUint32(pointer, 48, true)
                view.setUint32(pointer + 16, event.id, true)
                view.setInt32(pointer + 20, -1, true)
                view.setInt16(pointer + 24, -1, true)
                view.setInt16(pointer + 26, -1, true)
                view.setInt16(pointer + 28, -1, true)
                view.setFloat64(pointer + 40, event.value, true)
            } else if (event.type === EVENT.MIDI) {
                view.setUint32(pointer, 24, true) // clap_event_midi: header, port_index @16, data @18
                this.bytes.set(event.data, pointer + 18)
            } else if (event.type === EVENT.NOTE_EXPRESSION) {
                // clap_event_note_expression: expression_id @16, note_id @20, port @24, channel @26, key @28, value @32
                view.setUint32(pointer, 40, true)
                view.setUint32(pointer + 16, 2, true)   // CLAP_NOTE_EXPRESSION_TUNING
                view.setInt32(pointer + 20, -1, true)   // note_id
                view.setInt16(pointer + 24, -1, true)   // port_index
                view.setInt16(pointer + 26, 0, true)    // channel
                view.setInt16(pointer + 28, event.key, true)
                view.setFloat64(pointer + 32, event.value, true)
            } else {
                view.setUint32(pointer, 40, true)
                view.setInt32(pointer + 16, -1, true)
                view.setInt16(pointer + 22, 0, true)
                view.setInt16(pointer + 24, event.key, true)
                view.setFloat64(pointer + 32, event.velocity ?? 0.8, true)
            }
        })
        this.count = events.length
    }

    // absolute sample times, in blocks of `block`; returns both channels and the last status
    run(events, until, block = BLOCK) {
        const queue = events.toSorted((first, second) => first.time - second.time)
        const left = [], right = []
        let status = 0
        while (this.position < until) {
            const frames = Math.min(block, until - this.position)
            const end = this.position + frames
            const chunk = []
            while (queue.length > 0 && queue[0].time < end) {
                const event = queue.shift()
                chunk.push({...event, time: event.time - this.position})
            }
            this.write(chunk)
            const view = this.view
            view.setBigUint64(this.process, BigInt(this.position), true)
            view.setUint32(this.process + 8, frames, true)
            view.setUint32(this.process + 12, 0, true)
            view.setUint32(this.process + 16, 0, true)
            view.setUint32(this.process + 20, this.audioOut, true)
            view.setUint32(this.process + 24, 0, true)
            view.setUint32(this.process + 28, 1, true)
            view.setUint32(this.process + 32, this.inEvents, true)
            view.setUint32(this.process + 36, this.outEvents, true)
            status = this.fn(this.plugin + 36)(this.plugin, this.process)
            left.push(...new Float32Array(this.memory.buffer, this.channels[0], frames))
            right.push(...new Float32Array(this.memory.buffer, this.channels[1], frames))
            this.count = 0
            this.out = []
            this.position = end
        }
        return {left, right, status}
    }

    time(blocks, block = BLOCK) {
        const view = this.view
        this.write([])
        view.setUint32(this.process + 8, block, true)
        view.setUint32(this.process + 12, 0, true)
        view.setUint32(this.process + 16, 0, true)
        view.setUint32(this.process + 20, this.audioOut, true)
        view.setUint32(this.process + 24, 0, true)
        view.setUint32(this.process + 28, 1, true)
        view.setUint32(this.process + 32, this.inEvents, true)
        view.setUint32(this.process + 36, this.outEvents, true)
        const started = performance.now()
        for (let i = 0; i < blocks; i++)
            this.fn(this.plugin + 36)(this.plugin, this.process)
        return (performance.now() - started) / blocks
    }

    paramInfo(index) {
        const info = this.malloc(1320)
        this.bytes.fill(0, info, info + 1320)
        assert.ok(this.fn(this.params + 4)(this.plugin, index, info), "params info")
        const view = this.view
        return {
            id: view.getUint32(info, true), flags: view.getUint32(info + 4, true),
            name: this.cstr(info + 12), module: this.cstr(info + 268),
            min: view.getFloat64(info + 1296, true), max: view.getFloat64(info + 1304, true),
            def: view.getFloat64(info + 1312, true)
        }
    }
    paramValue(id) {
        const pointer = this.malloc(8)
        assert.ok(this.fn(this.params + 8)(this.plugin, id, pointer), "params value")
        return this.view.getFloat64(pointer, true)
    }
    valueToText(id, value) {
        const text = this.malloc(256)
        assert.ok(this.fn(this.params + 12)(this.plugin, id, value, text, 256), "value to text")
        return this.cstr(text)
    }
    textToValue(id, text) {
        const pointer = this.malloc(8)
        const ok = this.fn(this.params + 16)(this.plugin, id, this.string(text), pointer)
        return ok ? this.view.getFloat64(pointer, true) : null
    }
    save() {
        this.written = []
        assert.ok(this.fn(this.state)(this.plugin, this.ostream), "state save")
        return Uint8Array.from(this.written)
    }
    load(bytes) {
        this.source = bytes
        this.cursor = 0
        return this.fn(this.state + 4)(this.plugin, this.istream)
    }
    receive(bytes) {
        this.sent = []
        this.bytes.set(bytes, this.scratch)
        const handled = this.fn(this.webview + 8)(this.plugin, this.scratch, bytes.length)
        return {handled, sent: this.sent}
    }
    page(type, fill = () => {}, size = 8) {
        const view = new DataView(new ArrayBuffer(size))
        view.setUint8(0, type)
        fill(view)
        return this.receive(new Uint8Array(view.buffer))
    }
    flush() {
        this.write([])
        this.out = []
        this.fn(this.params + 20)(this.plugin, this.inEvents, this.outEvents)
        return this.out
    }
}

const peak = samples => samples.reduce((max, value) => Math.max(max, Math.abs(value)), 0)
const rms = samples => Math.sqrt(samples.reduce((sum, value) => sum + value * value, 0) / Math.max(1, samples.length))
// Spectral centroid of the mono mix over the first `frames` samples, Hz — to check the resampler keeps the pitch.
const centroid = (left, right, frames, rate) => {
    const n = Math.min(frames, left.length)
    let weighted = 0, total = 0
    const size = 4096
    const re = new Float64Array(size / 2), im = new Float64Array(size / 2)
    for (let start = 0; start + size <= n; start += size) {
        for (let k = 0; k < size / 2; k++) {re[k] = 0; im[k] = 0}
        for (let i = 0; i < size; i++) {
            const w = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / size)
            const x = (left[start + i] + right[start + i]) * 0.5 * w
            for (let k = 1; k < size / 2; k++) {
                const angle = 2 * Math.PI * k * i / size
                re[k] += x * Math.cos(angle)
                im[k] -= x * Math.sin(angle)
            }
        }
        for (let k = 1; k < size / 2; k++) {
            const power = re[k] * re[k] + im[k] * im[k]
            weighted += power * (k * rate / size)
            total += power
        }
    }
    return total > 0 ? weighted / total : 0
}

async function main() {
    const host = await Host.load(process.argv[2] ?? "build/faust-elements/web/faust-elements.wclap/module.wasm")
    console.log(`descriptor: ${host.id} "${host.name}" version ${host.version}`)

    // --- parameters --------------------------------------------------------------------------------------
    assert.equal(host.fn(host.params)(host.plugin), PARAMS, "params count")
    const infos = Array.from({length: PARAMS}, (_, i) => host.paramInfo(i))
    const byName = Object.fromEntries(infos.map(info => [info.name, info]))
    const model = byName["model"], modulation = byName["modulation"], lfo = byName["mod_frequency"]
    const space = byName["space"], bow = byName["bow"]
    assert.ok(model && modulation && lfo && space && bow, "known params present")
    assert.equal(model.module, "resonator")
    assert.equal(space.module, "space")
    assert.ok(model.flags & 0x1, "model is stepped")       // CLAP_PARAM_IS_STEPPED
    assert.ok(model.flags & 0x10000, "model is enum")      // CLAP_PARAM_IS_ENUM
    assert.equal(host.valueToText(model.id, 0), "Modal")
    assert.equal(host.valueToText(model.id, 1), "String")
    assert.equal(host.valueToText(model.id, 2), "Strings")
    assert.equal(host.textToValue(model.id, "Strings"), 2)
    assert.equal(host.valueToText(modulation.id, 1.5), "+1.50 st")
    assert.ok(host.valueToText(lfo.id, 0.5).endsWith("Hz"))
    assert.equal(bow.min, 0)
    assert.equal(bow.max, 1)
    assert.equal(bow.def, 0)
    console.log(`params: ${PARAMS}, e.g. model menu "Modal"/"String"/"Strings" in resonator, space ${space.min}..${space.max}`)

    // --- state -------------------------------------------------------------------------------------------
    const saved = host.save()
    assert.equal(saved.length, STATE_SIZE, "state size")
    assert.equal(String.fromCharCode(...saved.slice(0, 4)), "ELEM", "state magic")
    host.run([{type: EVENT.PARAM_VALUE, time: host.position, id: space.id, value: 1.9}], host.position + BLOCK, BLOCK)
    assert.equal(host.paramValue(space.id), 1.9)
    assert.ok(host.load(saved), "state load")
    assert.equal(host.paramValue(space.id), space.def, "state restores values")
    console.log(`state: ${STATE_SIZE} bytes, magic ELEM, round-trip ok`)

    // --- notes and audio ---------------------------------------------------------------------------------
    // A bowed note at A4 (69), sustained, at 48 kHz: sounds, finite, and its spectral centroid over the sustained
    // part matches the same render at 32 kHz host rate (the resampler keeps the pitch).
    const bowPatch = [{type: EVENT.PARAM_VALUE, time: host.position, id: bow.id, value: 0.85},
                      {type: EVENT.PARAM_VALUE, time: host.position, id: byName["strike"].id, value: 0},
                      {type: EVENT.PARAM_VALUE, time: host.position, id: byName["envelope"].id, value: 0.5}]
    host.run(bowPatch, host.position + BLOCK, BLOCK)
    const note = [{type: EVENT.NOTE_ON, time: host.position, key: 69, velocity: 0.9}]
    const played = host.run(note, host.position + SAMPLE_RATE)
    assert.ok(peak(played.left) > 0.01, "a note sounds")
    assert.ok(played.left.every(Number.isFinite), "output is finite")
    const host32 = await Host.load(process.argv[2] ?? "build/faust-elements/web/faust-elements.wclap/module.wasm", 32000)
    host32.run(bowPatch.map(e => ({...e, time: 0})), BLOCK, BLOCK)
    const played32 = host32.run([{type: EVENT.NOTE_ON, time: 0, key: 69, velocity: 0.9}], 32000)
    // skip the attack (first 0.2 s), measure the sustained part
    const skip48 = Math.floor(SAMPLE_RATE * 0.2), skip32 = Math.floor(32000 * 0.2)
    const c48 = centroid(played.left.slice(skip48), played.right.slice(skip48), SAMPLE_RATE - skip48, SAMPLE_RATE)
    const c32 = centroid(played32.left.slice(skip32), played32.right.slice(skip32), 32000 - skip32, 32000)
    assert.ok(Math.abs(c48 - c32) / c32 < 0.03, `resampler keeps the pitch (centroid ${c48.toFixed(1)} vs ${c32.toFixed(1)} Hz)`)
    console.log(`audio: bowed note peak ${peak(played.left).toFixed(3)}, centroid ${c48.toFixed(1)} Hz at 48k vs ${c32.toFixed(1)} Hz at 32k`)

    // release, then silence and sleep (the reverb tail is a few seconds; the plugin sleeps after half a second of
    // silence once no voice is active)
    const tail = host.run([{type: EVENT.NOTE_OFF, time: host.position, key: 69}], host.position + 8 * SAMPLE_RATE)
    assert.ok(peak(tail.left.slice(-SAMPLE_RATE)) < 1e-4, "release and tail end in silence")
    assert.equal(tail.status, STATUS.SLEEP, "sleeps after the tail")
    console.log(`audio: release ends in silence, sleeps (status ${tail.status})`)

    // a tuning note expression transposes: +12 st doubles the centroid
    host.run([{type: EVENT.NOTE_ON, time: host.position, key: 69, velocity: 0.9},
              {type: EVENT.NOTE_EXPRESSION, time: host.position, key: 69, value: 12}], host.position + SAMPLE_RATE / 2)
    const bent = host.run([], host.position + SAMPLE_RATE / 2)
    const bentCentroid = centroid(bent.left, bent.right, SAMPLE_RATE, SAMPLE_RATE)
    assert.ok(bentCentroid > c48 * 1.7, `tuning expression transposes (${bentCentroid.toFixed(0)} Hz vs ${c48.toFixed(0)} Hz)`)
    host.run([{type: EVENT.NOTE_OFF, time: host.position, key: 69}], host.position + 2 * SAMPLE_RATE)
    console.log(`audio: +12 st tuning expression -> centroid ${bentCentroid.toFixed(0)} Hz`)

    // the resonator model switches: five strings sound
    host.run([{type: EVENT.PARAM_VALUE, time: host.position, id: model.id, value: 2},
              {type: EVENT.NOTE_ON, time: host.position, key: 60, velocity: 0.9}], host.position + SAMPLE_RATE / 2)
    const strings = host.run([], host.position + SAMPLE_RATE / 2)
    assert.ok(peak(strings.left) > 0.01, "string model sounds")
    host.run([{type: EVENT.NOTE_OFF, time: host.position, key: 60},
              {type: EVENT.PARAM_VALUE, time: host.position, id: model.id, value: 0}], host.position + 2 * SAMPLE_RATE)
    console.log(`audio: model 2 (five strings) peak ${peak(strings.left).toFixed(3)}`)

    // MIDI note on
    const midi = host.run([{type: EVENT.MIDI, time: host.position, data: [0x90, 64, 100]}], host.position + SAMPLE_RATE / 4)
    assert.ok(peak(midi.left) > 0.001, "MIDI note on sounds")
    host.run([{type: EVENT.MIDI, time: host.position, data: [0x80, 64, 0]}], host.position + SAMPLE_RATE)
    console.log("audio: MIDI dialect works")

    // --- page protocol -----------------------------------------------------------------------------------
    const hello = host.page(PAGE.HELLO, () => {}, 4)
    assert.ok(hello.handled)
    const describe = hello.sent.find(message => message[0] === PLUGIN.DESCRIBE)
    const snapshot = hello.sent.find(message => message[0] === PLUGIN.SNAPSHOT)
    assert.ok(describe && snapshot, "hello answers describe + snapshot")
    const table = JSON.parse(new TextDecoder().decode(describe.slice(4)))
    assert.equal(table.length, PARAMS, "describe table size")
    assert.equal(table.find(p => p.label === "space").body, true)
    assert.equal(table.find(p => p.label === "bow").body, false)
    assert.equal(snapshot.length, 4 + STATE_SIZE - 12, "snapshot size (values only)")
    const values = hello.sent.find(message => message[0] === PLUGIN.VALUES)
    assert.ok(values, "hello answers with the values too")
    assert.equal(values[2] | values[3] << 8, PARAMS)

    // set from the page: one value event out, and the host sees the value
    const set = host.page(PAGE.SET, view => {
        view.setUint32(4, space.id, true)
        view.setFloat64(8, 1.5, true)
    }, 16)
    assert.ok(set.handled)
    const outEvents = host.flush()
    assert.ok(outEvents.some(e => e.type === EVENT.PARAM_VALUE && e.param === space.id && e.value === 1.5),
              "page set reaches the host")
    assert.equal(host.paramValue(space.id), 1.5)
    // host-driven change is reported to the page on poll
    host.run([{type: EVENT.PARAM_VALUE, time: host.position, id: space.id, value: 0.9}], host.position + BLOCK, BLOCK)
    const reported = host.page(PAGE.POLL, () => {}, 4)
    const valuesMessage = reported.sent.find(message => message[0] === PLUGIN.VALUES)
    assert.ok(valuesMessage, "host change reported on poll")
    assert.equal(host.paramValue(space.id), 0.9)

    // a patch from the page: name + 21 f64 values, all applied
    const name = "test"
    const patchSize = 4 + name.length + PARAMS * 8
    const patch = host.page(PAGE.PATCH, view => {
        view.setUint16(2, name.length, true)
        for (let i = 0; i < name.length; i++) view.setUint8(4 + i, name.charCodeAt(i))
        for (let i = 0; i < PARAMS; i++) view.setFloat64(4 + name.length + 8 * i, infos[i].max, true)
    }, patchSize)
    assert.ok(patch.handled)
    assert.equal(host.paramValue(space.id), space.max, "patch applied")

    // notes from the page (the editor's keyboard): 11 note on, 12 note off
    assert.ok(host.page(PAGE.NOTE_ON, view => {
        view.setUint32(4, 60, true)
        view.setFloat64(8, 0.8, true)
    }, 16).handled, "page note on")
    const pageNote = host.run([], host.position + SAMPLE_RATE / 2)
    assert.ok(peak(pageNote.left) > 0.01, "page note on sounds")
    assert.ok(host.page(PAGE.NOTE_OFF, view => view.setUint32(4, 60, true), 8).handled, "page note off")
    host.run([], host.position + 2 * SAMPLE_RATE)

    // hover and panic
    const hover = host.page(PAGE.HOVER, view => view.setUint32(4, space.id, true), 8)
    assert.ok(hover.handled && host.hovered === space.id, "hover reported")
    assert.ok(host.page(PAGE.PANIC, () => {}, 4).handled, "panic")
    const afterPanic = host.run([{type: EVENT.NOTE_ON, time: host.position, key: 60, velocity: 0.8}],
                                 host.position + BLOCK, BLOCK)
    assert.ok(peak(afterPanic.left) > 0.001, "a note after panic sounds")
    host.run([{type: EVENT.NOTE_OFF, time: host.position, key: 60}], host.position + SAMPLE_RATE)
    console.log("page: hello/describe/snapshot, poll values, set, patch, notes, hover, panic all ok")

    // --- timing: an 8-note chord, modal resonator (the heaviest) -----------------------------------------
    host.run([{type: EVENT.PARAM_VALUE, time: host.position, id: model.id, value: 0}], host.position + BLOCK, BLOCK)
    const chord = [48, 52, 55, 59, 60, 64, 67, 72].map((key, i) => ({type: EVENT.NOTE_ON, time: i * 16, key, velocity: 0.8}))
    host.run(chord, host.position + BLOCK, BLOCK)
    const ms = host.time(200, BLOCK)
    const realtime = (BLOCK / SAMPLE_RATE) * 1000
    console.log(`dsp: ${ms.toFixed(3)} ms per ${BLOCK}-sample block (${(100 * ms / realtime).toFixed(1)}% of realtime) ` +
                `with an 8-note chord`)
    assert.ok(ms < realtime, "runs in realtime")

    console.log("all checks passed")
}

main().catch(error => {
    console.error(error)
    process.exit(1)
})
