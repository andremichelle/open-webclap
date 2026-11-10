// OpenRB in node: a minimal CLAP host with audio and a transport runs the seven plugins, each in its own bundle's
// module, and checks what
// they do: the 303's pitch, accent, slide, release and its patterns on the transport, the effects on test signals
// (the delay's echo at its length, the PCF following the beat, the compressor's gain, the distortion's curve), the
// latency, repeatability, the state and the page's messages.
// Usage: node check.mjs [<the bundles' directory>]   (default build/openrb/web, its <id>.wclap/module.wasm)
import assert from "node:assert/strict"
import {readFileSync} from "node:fs"

const SAMPLE_RATE = 48000
const BLOCK = 128
const EVENT = {NOTE_ON: 0, NOTE_OFF: 1, PARAM_VALUE: 5, MIDI: 10}
const VALTYPE = {i32: 0x7f, i64: 0x7e, f32: 0x7d, f64: 0x7c}
const KIND = {BASS: 0, DIST: 1, PCF: 2, COMP: 3, DELAY: 4, TR808: 5, TR909: 6}
const IDS = ["studio.opendaw.openrb.bass", "studio.opendaw.openrb.distortion", "studio.opendaw.openrb.pcf",
    "studio.opendaw.openrb.compressor", "studio.opendaw.openrb.delay", "studio.opendaw.openrb.tr808",
    "studio.opendaw.openrb.tr909"]
const INSTRUMENT = kind => kind === KIND.BASS || kind === KIND.TR808 || kind === KIND.TR909
const PAGE = {POLL: 1, BEGIN: 2, SET: 3, END: 4, HOVER: 7, HELLO: 8, STEP: 10, HEADER: 11, PATTERN: 12, COMMIT: 13,
    KIT_DLL: 14, KIT_SLOT: 15}
// the drum machines' own parameters: Play, Pattern, Shuffle, Gain, then the machine's controllers
// a percentage parameter at the device's byte (the host sees 0..100 %, the device 0..127)
const pct = byte => byte * 100 / 127
const R = {PLAY: 0, PATTERN: 1, SHUFFLE: 2, GAIN: 3, ACCENT: 4}
const PLUGIN = {STATUS: 1, SNAPSHOT: 2}
const B = {TUNE: 0, CUTOFF: 1, RESONANCE: 2, ENVMOD: 3, DECAY: 4, ACCENT: 5, WAVE: 6, SOUND: 7, PLAY: 8, PATTERN: 9,
    SHUFFLE: 10, GAIN: 11}
const PATTERN_BYTES = 98, PATTERNS = 32

const trampoline = (params, results, fn) => {
    const name = text => [text.length, ...[...text].map(letter => letter.charCodeAt(0))]
    const section = (id, bytes) => [id, bytes.length, ...bytes]
    const type = [1, 0x60, params.length, ...params.map(kind => VALTYPE[kind]), results.length, ...results.map(kind => VALTYPE[kind])]
    const bytes = new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        ...section(1, type), ...section(2, [1, ...name("e"), ...name("f"), 0x00, 0x00]), ...section(7, [1, ...name("f"), 0x00, 0x00])])
    return new WebAssembly.Instance(new WebAssembly.Module(bytes), {e: {f: fn}}).exports.f
}

// the bundles in the order of KIND, each a module with its one plugin
const BUNDLES = ["openrb-bass", "openrb-distortion", "openrb-pcf", "openrb-compressor", "openrb-delay", "openrb-808", "openrb-909"]
const directory = process.argv[2] ?? new URL("../../build/openrb/web", import.meta.url).pathname
const modules = BUNDLES.map(id => new WebAssembly.Module(readFileSync(`${directory}/${id}.wclap/module.wasm`)))

class Host {
    // a 303 starts in pattern mode; notes: switched to Notes, as the note checks want it
    constructor(kind, notes = true) {
        const compiled = modules[kind]
        const imports = {}
        for (const entry of WebAssembly.Module.imports(compiled)) {
            (imports[entry.module] ??= {})[entry.name] = () => {throw new Error(`${entry.module}.${entry.name} called`)}
        }
        // WASI as a host gives it: no environment, output dropped
        const wasi = imports.wasi_snapshot_preview1 ?? {}
        let memory = null
        wasi.environ_sizes_get = (count, size) => {
            new DataView(memory.buffer).setUint32(count, 0, true)
            new DataView(memory.buffer).setUint32(size, 0, true)
            return 0
        }
        wasi.environ_get = () => 0
        wasi.fd_write = (_fd, iovs, count, written) => {
            const view = new DataView(memory.buffer)
            let total = 0
            for (let i = 0; i < count; i++) {total += view.getUint32(iovs + i * 8 + 4, true)}
            view.setUint32(written, total, true)
            return 0
        }
        const instance = new WebAssembly.Instance(compiled, imports)
        memory = instance.exports.memory
        instance.exports._initialize?.()
        const exports = instance.exports
        this.memory = exports.memory
        this.table = exports.__indirect_function_table
        this.malloc = exports.malloc
        this.free = exports.free
        this.sent = []
        this.dirty = 0
        this.shown = 0
        this.latencyChanged = 0
        this.position = 0
        this.playing = false
        this.tempo = 120
        this.beatBase = 0
        this.baseAt = 0
        this.kind = kind
        const entry = exports.clap_entry.value
        assert.ok(this.fn(entry + 12)(this.string("/")), "entry init")
        const factory = this.fn(entry + 20)(this.string("clap.plugin-factory"))
        this.count = this.fn(factory)(factory)
        const descriptor = this.fn(factory + 4)(factory, 0)
        this.id = this.cstr(this.u32(descriptor + 12))
        this.name = this.cstr(this.u32(descriptor + 16))
        this.features = []
        for (let pointer = this.u32(descriptor + 44); this.u32(pointer) !== 0; pointer += 4) {this.features.push(this.cstr(this.u32(pointer)))}
        this.createHost()
        this.plugin = this.fn(factory + 8)(factory, this.host, this.string(this.id))
        assert.ok(this.plugin, "create")
        assert.ok(this.fn(this.plugin + 8)(this.plugin), "init")
        const extension = name => this.fn(this.plugin + 40)(this.plugin, this.string(name))
        this.params = extension("clap.params")
        this.state = extension("clap.state")
        this.webview = extension("clap.webview/3")
        this.notePorts = extension("clap.note-ports")
        this.audioPorts = extension("clap.audio-ports")
        this.latency = extension("clap.latency")
        this.tail = extension("clap.tail")
        this.panel = extension("opendaw.device-panel/1")
        this.createProcess()
        assert.ok(this.fn(this.plugin + 16)(this.plugin, SAMPLE_RATE, 1, BLOCK), "activate")
        assert.ok(this.fn(this.plugin + 24)(this.plugin), "start_processing")
        if (kind === KIND.BASS && notes) {this.setParam(B.PLAY, 0)}
        if ((kind === KIND.TR808 || kind === KIND.TR909) && notes) {this.setParam(R.PLAY, 0)}
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
        const strings = ["openrb check", "open-webclap", "", "1"].map(text => this.string(text))
        this.view.setUint32(host, 1, true)
        this.view.setUint32(host + 4, 2, true)
        this.view.setUint32(host + 8, 0, true)
        this.view.setUint32(host + 12, 0, true)
        strings.forEach((pointer, index) => this.view.setUint32(host + 16 + index * 4, pointer, true))
        this.install(host + 32, ["i32", "i32"], ["i32"], (_host, id) => extensions.get(this.cstr(id)) ?? 0)
        this.install(host + 36, ["i32"], [], () => {})
        this.install(host + 40, ["i32"], [], () => {})
        this.install(host + 44, ["i32"], [], () => {})
        const params = extension("clap.params", 12)
        this.install(params, ["i32", "i32"], [], () => {})
        this.install(params + 4, ["i32", "i32", "i32"], [], () => {})
        this.install(params + 8, ["i32"], [], () => {})
        const state = extension("clap.state", 4)
        this.install(state, ["i32"], [], () => this.dirty++)
        const latency = extension("clap.latency", 4)
        this.install(latency, ["i32"], [], () => this.latencyChanged++)
        const gui = extension("clap.gui", 20)
        this.install(gui, ["i32"], [], () => {})
        this.install(gui + 4, ["i32", "i32", "i32"], ["i32"], () => 0)
        this.install(gui + 8, ["i32"], ["i32"], () => (this.shown++, 1))
        this.install(gui + 12, ["i32"], ["i32"], () => 0)
        this.install(gui + 16, ["i32", "i32"], [], () => {})
        const webview = extension("clap.webview/3", 4)
        this.install(webview, ["i32", "i32", "i32"], ["i32"], (_host, pointer, size) => {
            this.sent.push(this.bytes.slice(pointer, pointer + size))
            return 1
        })
        const hovered = extension("clap.param-hovered/1", 4)
        this.install(hovered, ["i32", "i32"], [], () => {})
        this.host = host
    }

    createProcess() {
        this.process = this.malloc(40)
        this.slots = this.malloc(48 * 512)
        this.inEvents = this.malloc(12)
        this.install(this.inEvents + 4, ["i32"], ["i32"], () => this.eventCount)
        this.install(this.inEvents + 8, ["i32", "i32"], ["i32"], (_list, index) => index < this.eventCount ? this.slots + index * 48 : 0)
        this.outEvents = this.malloc(8)
        this.install(this.outEvents + 4, ["i32", "i32"], ["i32"], () => 1)
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
        // stereo in and out, a clap_audio_buffer_t each (data32, data64, channel_count, latency, constant_mask)
        this.channels = [0, 1, 2, 3].map(() => this.malloc(BLOCK * 4))
        this.inBuffer = this.malloc(24)
        this.outBuffer = this.malloc(24)
        this.inPointers = this.malloc(8)
        this.outPointers = this.malloc(8)
        this.scratch = this.malloc(8192)
        this.eventCount = 0
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
                view.setInt32(pointer + 24, -1, true)
                view.setFloat64(pointer + 40, event.value, true)
            } else if (event.type === EVENT.MIDI) {
                view.setUint32(pointer, 24, true)
                event.data.forEach((byte, offset) => view.setUint8(pointer + 18 + offset, byte))
            } else {
                view.setUint32(pointer, 40, true)
                view.setInt32(pointer + 16, -1, true)
                view.setInt16(pointer + 22, 0, true)
                view.setInt16(pointer + 24, event.key, true)
                view.setFloat64(pointer + 32, event.velocity ?? 0.8, true)
            }
        })
        this.eventCount = events.length
    }

    // absolute sample times, split into blocks like a host does; input(frame) gives the effects' input; answers the
    // output, left and right
    run(events, frames, input = () => 0) {
        const queue = events.toSorted((first, second) => first.time - second.time)
        const until = this.position + frames
        const left = new Float32Array(frames), right = new Float32Array(frames)
        let at = 0
        while (this.position < until) {
            const end = this.position + BLOCK
            const block = []
            while (queue.length > 0 && queue[0].time < end) {
                const event = queue.shift()
                block.push({...event, time: Math.max(0, event.time - this.position)})
            }
            this.write(block)
            const view = this.view
            const [inL, inR, outL, outR] = this.channels
            const inputs = new Float32Array(this.memory.buffer, inL, BLOCK), inputs2 = new Float32Array(this.memory.buffer, inR, BLOCK)
            for (let i = 0; i < BLOCK; i++) {
                const value = input(this.position + i)
                inputs[i] = Array.isArray(value) ? value[0] : value
                inputs2[i] = Array.isArray(value) ? value[1] : value
            }
            view.setUint32(this.inPointers, inL, true)
            view.setUint32(this.inPointers + 4, inR, true)
            view.setUint32(this.outPointers, outL, true)
            view.setUint32(this.outPointers + 4, outR, true)
            for (const [buffer, pointers] of [[this.inBuffer, this.inPointers], [this.outBuffer, this.outPointers]]) {
                this.bytes.fill(0, buffer, buffer + 24)
                view.setUint32(buffer, pointers, true)
                view.setUint32(buffer + 8, 2, true)
            }
            const effect = !INSTRUMENT(this.kind)
            view.setBigUint64(this.process, BigInt(this.position), true)
            view.setUint32(this.process + 8, BLOCK, true)
            view.setUint32(this.process + 12, this.writeTransport(), true)
            view.setUint32(this.process + 16, effect ? this.inBuffer : 0, true)
            view.setUint32(this.process + 20, this.outBuffer, true)
            view.setUint32(this.process + 24, effect ? 1 : 0, true)
            view.setUint32(this.process + 28, 1, true)
            view.setUint32(this.process + 32, this.inEvents, true)
            view.setUint32(this.process + 36, this.outEvents, true)
            assert.equal(this.fn(this.plugin + 36)(this.plugin, this.process), 1, "process continues")
            const outputs = new Float32Array(this.memory.buffer, outL, BLOCK), outputs2 = new Float32Array(this.memory.buffer, outR, BLOCK)
            const count = Math.min(BLOCK, frames - at)
            left.set(outputs.subarray(0, count), at)
            right.set(outputs2.subarray(0, count), at)
            at += count
            this.eventCount = 0
            this.position = end
        }
        return [left, right]
    }

    setParam(id, value) {return this.run([{time: this.position, type: EVENT.PARAM_VALUE, id, value}], BLOCK)}
    value(id) {
        const pointer = this.scratch
        assert.ok(this.fn(this.params + 8)(this.plugin, id, pointer), "get_value")
        return this.view.getFloat64(pointer, true)
    }
    text(id, value) {
        const pointer = this.scratch + 16
        assert.ok(this.fn(this.params + 12)(this.plugin, id, value, pointer, 64), "value_to_text")
        return this.cstr(pointer)
    }
    parse(id, text) {
        const pointer = this.scratch + 16, out = this.scratch
        this.bytes.set([...new TextEncoder().encode(text), 0], pointer)
        assert.ok(this.fn(this.params + 16)(this.plugin, id, pointer, out), `text_to_value ${text}`)
        return this.view.getFloat64(out, true)
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
        // a large message (a kit's sample) gets a buffer of its own
        const at = bytes.length <= 7000 ? this.scratch + 1024 : this.malloc(bytes.length)
        this.bytes.set(bytes, at)
        const handled = this.fn(this.webview + 8)(this.plugin, at, bytes.length)
        if (at !== this.scratch + 1024) {this.free(at)}
        return {handled, sent: this.sent}
    }
    latencyFrames() {return this.fn(this.latency)(this.plugin)}
    tailFrames() {return this.fn(this.tail)(this.plugin) >>> 0}
    // the host's transport per block: playing, tempo, the beat position (beatBase plus the samples since baseAt)
    writeTransport() {
        this.transportPtr ??= this.malloc(104)
        const view = this.view, pointer = this.transportPtr
        this.bytes.fill(0, pointer, pointer + 104)
        view.setUint32(pointer, 104, true)
        view.setUint16(pointer + 10, 9, true)
        view.setUint32(pointer + 16, (this.playing ? 16 : 0) | 1 | 2 | 8, true)
        const beats = this.beatBase + (this.position - this.baseAt) / SAMPLE_RATE * this.tempo / 60
        view.setBigInt64(pointer + 24, BigInt(Math.round(beats * 2 ** 31)), true)
        view.setFloat64(pointer + 40, this.tempo, true)
        view.setUint16(pointer + 100, 4, true)
        view.setUint16(pointer + 102, 4, true)
        return pointer
    }
    start(beats = 0) {
        this.playing = true
        this.beatBase = beats
        this.baseAt = this.position
    }
    stop() {this.playing = false}
}

// --- signals -------------------------------------------------------------------------------------------------------

const on = (time, key, velocity = 0.7) => ({time, type: EVENT.NOTE_ON, key, velocity})
const off = (time, key) => ({time, type: EVENT.NOTE_OFF, key, velocity: 0})
const peak = (samples, from = 0, to = samples.length) => {
    let value = 0
    for (let i = from; i < to; i++) {value = Math.max(value, Math.abs(samples[i]))}
    return value
}
const rms = (samples, from = 0, to = samples.length) => {
    let sum = 0
    for (let i = from; i < to; i++) {sum += samples[i] * samples[i]}
    return Math.sqrt(sum / Math.max(1, to - from))
}
// the fundamental by autocorrelation between 30 and 1000 Hz: the first lag that correlates nearly as well as the best
// (a longer lag, a subharmonic, correlates as well)
const pitch = (samples, from, length = 8192) => {
    const x = samples.subarray(from, from + length)
    const low = Math.floor(SAMPLE_RATE / 1000), high = Math.ceil(SAMPLE_RATE / 30)
    const r = new Float64Array(high + 1)
    for (let lag = low; lag <= high; lag++) {
        let sum = 0
        for (let i = 0; i + lag < x.length; i++) {sum += x[i] * x[i + lag]}
        r[lag] = sum / (x.length - lag)
    }
    const best = Math.max(...r.subarray(low))
    for (let lag = low + 1; lag < high; lag++) {
        if (r[lag] >= 0.9 * best && r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1]) {return SAMPLE_RATE / lag}
    }
    return 0
}
const midiHz = key => 440 * 2 ** ((key - 69) / 12)
const cents = (a, b) => 1200 * Math.log2(a / b)

const pageParam = (type, id, value) => {
    const view = new DataView(new ArrayBuffer(type === PAGE.SET ? 16 : 8))
    view.setUint8(0, type)
    view.setUint32(4, id, true)
    if (type === PAGE.SET) {view.setFloat64(8, value, true)}
    return new Uint8Array(view.buffer)
}
// a 303 pattern: the steps as [note, slide, accent, up, down, rest]
const patternBytes = (steps, length = 16, shuffle = 0) => {
    const bytes = new Uint8Array(PATTERN_BYTES)
    bytes[0] = shuffle
    bytes[1] = length
    for (let i = 0; i < 16; i++) {bytes.set(steps[i] ?? [0, 0, 0, 0, 0, 1], 2 + i * 6)}
    return bytes
}
const patternMessage = (index, bytes) => {
    const message = new Uint8Array(4 + PATTERN_BYTES)
    message[0] = PAGE.PATTERN
    message[1] = index
    message.set(bytes, 4)
    return message
}

let passed = 0
const check = (name, test) => {
    test()
    passed++
    console.log(`ok  ${name}`)
}

const BEAT = SAMPLE_RATE / 2 // 120 bpm
const STEP16 = BEAT / 4

// --- the module ----------------------------------------------------------------------------------------------------

check("seven bundles of one plugin each, three instruments and four effects, each with its device panel and width, no window", () => {
    for (let kind = 0; kind < 7; kind++) {
        const host = new Host(kind)
        assert.equal(host.count, 1)
        assert.equal(host.id, IDS[kind])
        assert.ok(host.features.includes(INSTRUMENT(kind) ? "instrument" : "audio-effect"), host.features.join())
        assert.ok(host.features.includes("opendaw:device-panel"))
        const width = Number(host.features.find(feature => feature.startsWith("opendaw:device-panel-width:")).split(":")[2])
        assert.ok(width > 150 && width < 1400, `${host.name} width ${width}`)
        const out = host.scratch
        assert.ok(host.fn(host.panel + 4)(host.plugin, 176, out))
        assert.equal(host.u32(out), width, "get_width(176) as the feature")
        assert.equal(host.notePorts !== 0 && host.fn(host.notePorts)(host.plugin, true) === 1, INSTRUMENT(kind))
        assert.equal(host.fn(host.audioPorts)(host.plugin, true), INSTRUMENT(kind) ? 0 : 1)
        assert.equal(host.fn(host.audioPorts)(host.plugin, false), 1)
        assert.equal(host.fn(host.plugin + 40)(host.plugin, host.string("clap.gui")), 0, "the panel is the only view")
    }
})

// --- the 303 -------------------------------------------------------------------------------------------------------

check("303: plays its patterns by default (Play: Pattern)", () => {
    const host = new Host(KIND.BASS, false)
    assert.equal(host.value(B.PLAY), 1)
    host.start(0)
    assert.ok(peak(host.run([], BEAT * 4)[0]) > 0.1, "A1 plays on the transport")
})

check("303: a note plays at its pitch (C2 is the lowest C at the middle tune), the tune transposes", () => {
    for (const [key, tune, semis] of [[36, 0, 0], [48, 0, 0], [55, 0, 0], [45, -12, -12], [45, 12, 12], [41, 7, 7]]) {
        const host = new Host(KIND.BASS)
        host.setParam(B.TUNE, tune)
        host.setParam(B.CUTOFF, 100)
        host.setParam(B.DECAY, 100)
        const [left, right] = host.run([on(host.position, key)], SAMPLE_RATE / 2)
        assert.deepEqual(left, right, "mono to both sides")
        const hz = pitch(left, 2000)
        assert.ok(Math.abs(cents(hz, midiHz(key + semis))) < 25, `key ${key} tune ${tune}: ${hz.toFixed(1)} Hz`)
        assert.equal(host.text(B.TUNE, tune), semis === 0 ? "0 st" : `${semis > 0 ? "+" : ""}${semis} st`)
    }
})

check("303: velocity above 100 accents (louder), the release ends the note", () => {
    const level = velocity => {
        const host = new Host(KIND.BASS)
        const [left] = host.run([on(0, 48, velocity), off(STEP16 * 2, 48)], SAMPLE_RATE)
        return {body: peak(left, 0, STEP16 * 2), tail: peak(left, STEP16 * 2 + 4800)}
    }
    const plain = level(100 / 127), accented = level(1)
    assert.ok(accented.body > plain.body * 1.3, `accent ${accented.body} plain ${plain.body}`)
    assert.ok(plain.tail < plain.body * 0.02, `released: ${plain.tail} after ${plain.body}`)
})

check("303: a legato note slides (no new attack), a separate one starts over", () => {
    const run = (legato, cutoff = 0) => {
        const host = new Host(KIND.BASS)
        host.setParam(B.DECAY, 0)
        host.setParam(B.ENVMOD, 100)
        host.setParam(B.CUTOFF, pct(cutoff))
        const second = STEP16 * 4
        const events = legato ? [on(0, 48), on(second, 55), off(second + 100, 48), off(second * 2, 55)]
            : [on(0, 48), off(second - 200, 48), on(second, 55), off(second * 2, 55)]
        const [left] = host.run(events, second * 2)
        return {left, second}
    }
    const slide = run(true), attack = run(false)
    const after = ({left, second}) => rms(left, second + 300, second + 1500)
    // with decay at its shortest the filter envelope is spent: a slide keeps it closed, a new note opens it again
    assert.ok(after(attack) > after(slide) * 1.5, `new attack ${after(attack)} slide ${after(slide)}`)
    // the slide reaches the new pitch (with the filter open, to hear it)
    const open = run(true, 127)
    assert.ok(Math.abs(cents(pitch(open.left, open.second + 4800, 4096), midiHz(55))) < 30)
})

check("303: MIDI notes as well as CLAP notes", () => {
    const host = new Host(KIND.BASS)
    const [left] = host.run([{time: 0, type: EVENT.MIDI, data: [0x90, 48, 90]}, {time: STEP16, type: EVENT.MIDI, data: [0x80, 48, 0]}], SAMPLE_RATE / 2)
    assert.ok(peak(left, 0, STEP16) > 0.1)
    assert.ok(peak(left, STEP16 + 6000) < 0.01)
})

check("303: Gain scales the output in dB (0 dB the engine's own level)", () => {
    const render = db => {
        const host = new Host(KIND.BASS)
        host.setParam(B.GAIN, db)
        return host.run([on(host.position, 48), off(host.position + STEP16 * 2, 48)], STEP16 * 3)[0]
    }
    const unity = peak(render(0)), down = peak(render(-12)), up = peak(render(6))
    assert.ok(Math.abs(20 * Math.log10(down / unity) + 12) < 0.2, `-12 dB: ${down / unity}`)
    assert.ok(Math.abs(20 * Math.log10(up / unity) - 6) < 0.2, `+6 dB: ${up / unity}`)
    const host = new Host(KIND.BASS)
    assert.equal(host.text(B.GAIN, -6), "-6 dB")
    assert.equal(host.text(B.GAIN, 3), "+3 dB")
    assert.equal(host.parse(B.GAIN, "-9 dB"), -9)
})

check("303: the same notes give the same samples (sound models 2.0 and Vintage differ)", () => {
    const render = vintage => {
        const host = new Host(KIND.BASS)
        host.setParam(B.SOUND, vintage)
        return host.run([on(0, 40, 1), off(STEP16, 40), on(STEP16 * 2, 52, 0.5), off(STEP16 * 3, 52)], SAMPLE_RATE)[0]
    }
    assert.deepEqual(render(0), render(0))
    assert.notDeepEqual(render(0), render(1))
})

check("303: patterns play on the transport from bar to bar, a new pattern at the next bar line, nothing while stopped", () => {
    const host = new Host(KIND.BASS)
    host.setParam(B.PLAY, 1)
    // pattern A1: a note on steps 1 and 9; A2: on step 5 only
    const a1 = patternBytes({0: [0, 0, 0, 0, 0, 0], 8: [7, 0, 1, 0, 0, 0]})
    const a2 = patternBytes({4: [12, 0, 0, 0, 0, 0]})
    assert.ok(host.receive(patternMessage(0, a1)).handled)
    assert.ok(host.receive(patternMessage(1, a2)).handled)
    host.receive(new Uint8Array([PAGE.COMMIT]))
    assert.equal(host.dirty, 1, "a commit marks the state dirty")
    const stopped = host.run([], BEAT * 4)[0]
    assert.ok(peak(stopped) < 1e-4, "silent while stopped (the engine's denormal guard aside)")
    host.start(0)
    const bar = BEAT * 4
    const [first] = host.run([], bar)
    const loud = (samples, step) => peak(samples, step * STEP16 + 400, step * STEP16 + STEP16 / 2)
    assert.ok(loud(first, 0) > 0.1 && loud(first, 8) > 0.1, "steps 1 and 9")
    assert.ok(loud(first, 4) < 0.02 && loud(first, 12) < 0.02, "rests")
    // the pitch of step 1 (note 0, C2)
    assert.ok(Math.abs(cents(pitch(first, 400, 2048), midiHz(36))) < 40)
    host.setParam(B.PATTERN, 1) // mid bar: A1 plays on until the bar line
    const [second] = host.run([], bar - BLOCK)
    const shifted = Array.from(second)
    const [third] = host.run([], bar)
    assert.ok(loud(third, 4) > 0.1 && loud(third, 0) < 0.02 && loud(third, 8) < 0.02, "A2 from the next bar")
    assert.ok(shifted.length > 0)
    // the commit made the next poll a snapshot (the other page takes the edit), the one after a status: playing, A2,
    // the step it played last
    assert.equal(host.receive(new Uint8Array([PAGE.POLL])).sent[0][0], PLUGIN.SNAPSHOT)
    const {sent} = host.receive(new Uint8Array([PAGE.POLL]))
    assert.equal(sent.length, 1)
    assert.equal(sent[0][0], PLUGIN.STATUS)
    assert.equal(sent[0][2] & 1, 1, "playing")
    assert.equal(sent[0][3], 1, "pattern A2")
    assert.ok(sent[0][4] < 16, "a step")
    host.stop()
    const [after] = host.run([], BEAT * 2)
    assert.ok(peak(after, 6000) < 0.01, "stop releases")
})

check("303: bank A holds factory patterns, every one plays on the transport", () => {
    const snapshot = new Host(KIND.BASS).receive(new Uint8Array([PAGE.HELLO])).sent[0]
    const patterns = snapshot.subarray(snapshot.length - PATTERNS * PATTERN_BYTES)
    const played = p => [...Array(16).keys()].filter(i => patterns[p * PATTERN_BYTES + 2 + i * 6 + 5] === 0).length
    for (let p = 0; p < 8; p++) {assert.ok(played(p) >= 8, `A${p + 1}: ${played(p)} notes`)}
    for (let p = 8; p < PATTERNS; p++) {assert.equal(played(p), 0, "the other banks start empty")}
    for (let p = 0; p < 8; p++) {
        const host = new Host(KIND.BASS)
        host.setParam(B.PLAY, 1)
        host.setParam(B.PATTERN, p)
        host.start(0)
        assert.ok(peak(host.run([], BEAT * 4)[0]) > 0.1, `A${p + 1} plays`)
    }
})

check("303: Shuffle moves the second 16th of each 8th later in every pattern, at 0 nothing moves", () => {
    const onset = (shuffle, on = 1) => {
        const host = new Host(KIND.BASS)
        host.setParam(B.PLAY, 1)
        host.setParam(B.SHUFFLE, pct(shuffle))
        host.receive(patternMessage(0, patternBytes({1: [0, 0, 1, 0, 0, 0]}, 16, on)))
        host.start(0)
        const [left] = host.run([], BEAT)
        return left.findIndex(x => Math.abs(x) > 0.05)
    }
    const straight = onset(0), shuffled = onset(127)
    assert.ok(Math.abs(straight - STEP16) < 400, `straight at ${straight}`)
    // 127 / 6 = 21 ticks of 48 a 16th later
    const expected = STEP16 + 21 / 48 * STEP16
    assert.ok(Math.abs(shuffled - expected) < 400, `shuffled at ${shuffled}, expected ${expected}`)
    assert.ok(Math.abs(onset(127, 0) - expected) < 400, "the pattern's old switch has no say")
})

check("303: notes are ignored in pattern mode, the pattern is silent in note mode", () => {
    const host = new Host(KIND.BASS)
    host.setParam(B.PLAY, 1)
    const [left] = host.run([on(host.position, 48), off(host.position + STEP16, 48)], BEAT)
    assert.ok(peak(left) < 1e-4)
    host.setParam(B.PLAY, 0)
    host.receive(patternMessage(0, patternBytes({0: [0, 0, 0, 0, 0, 0]})))
    host.start(0)
    assert.ok(peak(host.run([], BEAT * 4)[0]) < 1e-4)
})

check("303: the state keeps the parameters and the patterns, a step edit changes its 6 bytes", () => {
    const host = new Host(KIND.BASS)
    host.setParam(B.CUTOFF, 99)
    host.setParam(B.PATTERN, 17)
    host.receive(patternMessage(5, patternBytes({3: [11, 1, 1, 1, 0, 0]}, 12, 1)))
    const saved = host.save()
    assert.equal(saved.length, 8 + 12 * 8 + PATTERNS * PATTERN_BYTES)
    const step = new Uint8Array([PAGE.STEP, 5, 4, 0, 2, 0, 1, 0, 1, 0])
    assert.ok(host.receive(step).handled)
    const edited = host.save()
    const changed = [...edited].map((byte, i) => byte !== saved[i] ? i : -1).filter(i => i >= 0)
    const base = 8 + 12 * 8 + 5 * PATTERN_BYTES + 2 + 4 * 6
    assert.ok(changed.every(i => i >= base && i < base + 6), `changed ${changed}`)
    const other = new Host(KIND.BASS)
    assert.ok(other.load(edited))
    assert.equal(other.value(B.CUTOFF), 99)
    assert.equal(other.value(B.PATTERN), 17)
    assert.equal(other.text(B.PATTERN, 17), "C2")
    assert.equal(edited[8 + 12 * 8 + 5 * PATTERN_BYTES], 1, "a pattern's shuffle flag is always on")
    assert.equal(other.parse(B.PATTERN, "d8"), 31)
    assert.deepEqual(other.save(), edited)
    // the snapshot carries the patterns
    const {sent} = other.receive(new Uint8Array([PAGE.HELLO]))
    const snapshot = sent[0]
    assert.equal(snapshot[0], PLUGIN.SNAPSHOT)
    const patterns = snapshot.subarray(snapshot.length - PATTERNS * PATTERN_BYTES)
    assert.deepEqual(patterns.subarray(5 * PATTERN_BYTES, 6 * PATTERN_BYTES), edited.subarray(base - 2 - 4 * 6, base - 2 - 4 * 6 + PATTERN_BYTES))
    // a bad state is refused
    assert.equal(new Host(KIND.BASS).load(edited.subarray(0, 40)), 0)
    assert.equal(new Host(KIND.DIST).load(edited), 0, "another kind's state")
})

check("303: the page's parameter gestures reach the host, hover too", () => {
    const host = new Host(KIND.BASS)
    assert.ok(host.receive(pageParam(PAGE.BEGIN, B.RESONANCE)).handled)
    assert.ok(host.receive(pageParam(PAGE.SET, B.RESONANCE, 12)).handled)
    assert.ok(host.receive(pageParam(PAGE.END, B.RESONANCE)).handled)
    assert.equal(host.value(B.RESONANCE), 12)
    assert.equal(host.dirty, 1)
    assert.ok(host.receive(pageParam(PAGE.HOVER, B.RESONANCE)).handled)
})

// --- the drum machines ----------------------------------------------------------------------------------------------

const DRUM_PATTERN_BYTES = 194
// a made-up test sample: a decaying tone, 16-bit at 44.1 kHz
const testSample = (hz, seconds = 0.3) => Int16Array.from({length: Math.round(44100 * seconds)},
    (_, i) => Math.round(20000 * Math.exp(-i / 44100 * 12) * Math.sin(2 * Math.PI * hz * i / 44100)))
const kitSlot = (id, audio) => {
    const message = new Uint8Array(8 + audio.length * 2)
    const view = new DataView(message.buffer)
    message[0] = PAGE.KIT_SLOT
    view.setUint16(2, id, true)
    view.setUint32(4, audio.length, true)
    new Int16Array(message.buffer, 8, audio.length).set(audio)
    return message
}
// every slot of the machine with a test sample
const loadKit = host => {
    const [first, last] = host.kind === KIND.TR808 ? [1000, 1023] : [1025, 1045]
    for (let id = first; id <= last; id++) {assert.ok(host.receive(kitSlot(id, testSample(80 + (id - first) * 40))).handled)}
}

for (const [kind, name] of [[KIND.TR808, "808"], [KIND.TR909, "909"]]) {
    check(`${name}: the factory kit sounds from the start and is saved as a reference, a slot the user brings is saved whole`, () => {
        const host = new Host(kind)
        const slots = kind === KIND.TR808 ? 24 : 21
        assert.equal(host.receive(new Uint8Array([PAGE.POLL])).sent[0][5], slots, "every slot holds its factory sample")
        for (const key of [36, 38, 41, 45, 48, 37, 39, 42, 46, 49]) {
            const [left] = host.run([on(host.position, key)], 8000)
            assert.ok(peak(left) > 0.005, `key ${key} (factory)`)
        }
        const fresh = host.save()
        assert.ok(fresh.length < 8000, `a fresh state holds no samples (${fresh.length} bytes)`)
        const view = new DataView(fresh.buffer, fresh.byteOffset)
        for (let i = 0; i < slots; i++) {assert.equal(view.getUint32(fresh.length - 4 * (slots - i), true), 0xFFFFFFFF)}
        const first = kind === KIND.TR808 ? 1000 : 1025
        assert.ok(host.receive(kitSlot(first, testSample(60))).handled)
        const mixed = host.save()
        assert.equal(mixed.length, fresh.length + 2 * Math.round(44100 * 0.3), "the user's slot is saved whole, the others stay references")
        const other = new Host(kind)
        assert.ok(other.load(mixed))
        assert.deepEqual(other.save(), mixed)
    })

    check(`${name}: a kit's slots sound on General MIDI's drum keys, velocity above 100 louder`, () => {
        const host = new Host(kind)
        loadKit(host)
        assert.equal(host.receive(new Uint8Array([PAGE.POLL])).sent[0][0], PLUGIN.SNAPSHOT, "a kit change sends the snapshot")
        assert.equal(host.receive(new Uint8Array([PAGE.POLL])).sent[0][5], kind === KIND.TR808 ? 24 : 21)
        for (const key of [36, 38, 41, 45, 48, 37, 39, 42, 46, 49]) {
            const [left] = host.run([on(host.position, key)], 8000)
            assert.ok(peak(left) > 0.005, `key ${key}`)
        }
        const level = velocity => peak(host.run([on(host.position, 36, velocity)], 8000)[0])
        assert.ok(level(1) > level(0.6) * 1.05, "accented louder")
        assert.ok(host.dirty >= 1, "a kit change marks the state dirty")
        assert.equal(host.receive(new Uint8Array([PAGE.KIT_DLL, 0, 0, 0, 77, 90, 1, 2, 3])).handled, 0, "not a ReBirth.dll")
    })

    check(`${name}: bank A's factory patterns play on the transport, the kit and the patterns go with the state`, () => {
        const host = new Host(kind, false)
        assert.equal(host.value(R.PLAY), 1, "Pattern by default")
        loadKit(host)
        host.start(0)
        const [left] = host.run([], BEAT * 4)
        assert.ok(peak(left, 0, STEP16) > 0.005, "a hit on the first step")
        const saved = host.save()
        const other = new Host(kind, false)
        assert.ok(other.load(saved))
        assert.deepEqual(other.save(), saved)
        assert.equal(other.receive(new Uint8Array([PAGE.HELLO])).sent[0][5], kind === KIND.TR808 ? 24 : 21, "the kit came back")
        other.start(0)
        assert.ok(peak(other.run([], BEAT)[0], 0, STEP16) > 0.005)
        // a step edit: the snare on step 2 of A5 changes one byte of the pattern
        const step = new Uint8Array(16)
        step[0] = PAGE.STEP; step[1] = 4; step[2] = 1
        step[4 + 2] = 1
        assert.ok(other.receive(step).handled)
        const edited = other.save()
        const changed = [...edited].map((byte, i) => byte !== saved[i] ? i : -1).filter(i => i >= 0)
        assert.equal(changed.length, 1, `changed ${changed}`)
    })
}

// --- the effects ---------------------------------------------------------------------------------------------------

const sine = (hz, amplitude) => frame => amplitude * Math.sin(2 * Math.PI * hz * frame / SAMPLE_RATE)

check("effects: report their latency, the input comes out after it (the distortion at its gentlest)", () => {
    for (const kind of [KIND.DIST, KIND.PCF, KIND.COMP, KIND.DELAY]) {
        const host = new Host(kind)
        const latency = host.latencyFrames()
        assert.ok(latency > 64 && latency < 200, `latency ${latency}`)
    }
    const host = new Host(KIND.COMP)
    host.setParam(0, 0) // no amount: unity
    const latency = host.latencyFrames()
    const start = host.position
    const [left] = host.run([], 4096, frame => frame === start + 2048 ? 0.5 : 0)
    let at = 0
    for (let i = 0; i < left.length; i++) {if (Math.abs(left[i]) > Math.abs(left[at])) {at = i}}
    assert.ok(Math.abs(at - (2048 + latency)) <= 2, `impulse at ${at}, latency ${latency}`)
})

check("distortion: clips and shapes, harder with more amount at about the same level, its curve in the status", () => {
    const run = (amount, shape) => {
        const host = new Host(KIND.DIST)
        host.setParam(0, pct(amount))
        host.setParam(1, shape)
        const [left, right] = host.run([], SAMPLE_RATE / 2, sine(220, 0.3))
        return {left, right, host}
    }
    const gentle = run(10, 0), hard = run(127, 0), shaped = run(100, 21)
    assert.deepEqual(gentle.left, gentle.right)
    // the clipper's threshold falls as its gain rises: more amount squares the sine, the level stays
    const crest = samples => peak(samples, 4000) / rms(samples, 4000)
    assert.ok(crest(gentle.left) > 1.35 && crest(hard.left) < 1.1, `crest ${crest(gentle.left)} ${crest(hard.left)}`)
    assert.ok(Math.abs(rms(hard.left, 4000) / rms(gentle.left, 4000) - 1) < 0.5)
    assert.notDeepEqual(shaped.left, hard.left)
    const {sent} = shaped.host.receive(new Uint8Array([PAGE.POLL]))
    const status = sent[0], view = new DataView(status.buffer, status.byteOffset)
    assert.equal(status.length, 8 + 2 * 8 + 65 * 4)
    const curve = Array.from({length: 65}, (_, i) => view.getFloat32(8 + 16 + i * 4, true))
    assert.ok(Math.abs(curve[32]) < 0.05 && curve[64] > 0 && curve[0] < 0, curve.join())
    assert.ok(status[5] > 0, "the input meter moves")
    // Shape: the clipper, then the 32 curves
    const host = shaped.host
    assert.equal(host.text(1, 0), "Clip")
    assert.equal(host.text(1, 1), "1")
    assert.equal(host.text(1, 2), "2")
    assert.equal(host.text(1, 32), "32")
    assert.equal(host.parse(1, "Clip"), 0)
    assert.equal(host.parse(1, "1"), 1)
    assert.equal(host.text(1, host.parse(1, "17")), "17")
})

check("PCF: its pattern follows the beat while the transport plays, runs on while it stops", () => {
    const run = (playing, startBeat = 0) => {
        const host = new Host(KIND.PCF)
        if (playing) {host.start(startBeat)}
        const [left] = host.run([], BEAT * 4, frame => (frame % 100 < 50 ? 0.4 : -0.4))
        return {left, host}
    }
    const a = run(true), b = run(true)
    assert.deepEqual(a.left, b.left, "repeatable")
    // the envelope's steps: the level per 16th changes over the bar
    const levels = Array.from({length: 16}, (_, i) => rms(a.left, i * STEP16 + 200, (i + 1) * STEP16))
    assert.ok(Math.max(...levels) > Math.min(...levels) * 1.3, levels.map(x => x.toFixed(3)).join())
    // from another position of the song: another phase of the pattern
    const late = run(true, 1)
    assert.notDeepEqual(late.left, a.left)
    const free = run(false)
    const freeLevels = Array.from({length: 16}, (_, i) => rms(free.left, i * STEP16 + 200, (i + 1) * STEP16))
    assert.ok(Math.max(...freeLevels) > Math.min(...freeLevels) * 1.3, "runs on while stopped")
    const {sent} = a.host.receive(new Uint8Array([PAGE.POLL]))
    const status = sent[0], view = new DataView(status.buffer, status.byteOffset)
    assert.equal(status.length, 8 + 6 * 8 + 12 + 32)
    const length = view.getUint16(8 + 48 + 8, true), resolution = view.getUint16(8 + 48 + 10, true)
    assert.ok(length > 0 && [8, 16, 32].includes(resolution), `${length} ${resolution}`)
    assert.equal(a.host.text(3, 25), "25", "waves count from 1")
    assert.equal(a.host.value(3), 25)
})

check("compressor: turns a loud signal down, linked over both sides", () => {
    const host = new Host(KIND.COMP)
    host.setParam(0, 100)
    host.setParam(1, 0)
    const [left, right] = host.run([], SAMPLE_RATE, frame => [sine(110, 0.9)(frame), sine(110, 0.2)(frame)])
    const inL = 0.9 / Math.SQRT2, inR = 0.2 / Math.SQRT2
    const gainL = rms(left, SAMPLE_RATE / 2) / inL, gainR = rms(right, SAMPLE_RATE / 2) / inR
    assert.ok(gainL < 0.8, `gain ${gainL}`)
    assert.ok(Math.abs(gainL - gainR) < 0.05, `linked: ${gainL} ${gainR}`)
    const {sent} = host.receive(new Uint8Array([PAGE.POLL]))
    assert.ok(sent[0][6] > 0, "the input meter")
})

check("delay: echoes at its length (steps of 16ths or 8th triplets at the tempo), feedback repeats it, pan places it", () => {
    const echo = (steps, grid, tempo) => {
        const host = new Host(KIND.DELAY)
        host.tempo = tempo
        host.setParam(0, steps)
        host.setParam(1, grid)
        host.setParam(2, 0) // no feedback
        const latency = host.latencyFrames()
        const start = host.position
        const [left, right] = host.run([], SAMPLE_RATE * 2, frame => frame === start + 1000 ? 1 : 0)
        let at = 0
        for (let i = 3000; i < left.length; i++) {if (Math.abs(left[i]) > Math.abs(left[at])) {at = i}}
        return {at: at - 1000 - latency, left, right}
    }
    const sixteenths = echo(3, 0, 120)
    assert.ok(Math.abs(sixteenths.at - 3 * STEP16) < 3, `3/16 at 120: ${sixteenths.at}`)
    const triplets = echo(4, 1, 120)
    assert.ok(Math.abs(triplets.at - 4 * BEAT / 3) < 3, `4/8 T at 120: ${triplets.at}`)
    const slower = echo(2, 0, 90)
    assert.ok(Math.abs(slower.at - 2 * SAMPLE_RATE * 60 / 90 / 4) < 3, `2/16 at 90: ${slower.at}`)
    // feedback repeats it, the pan to the right leaves the left dry
    const host = new Host(KIND.DELAY)
    host.setParam(0, 2)
    host.setParam(2, pct(100))
    host.setParam(3, 63)
    const start = host.position
    const [left, right] = host.run([], SAMPLE_RATE * 2, frame => frame === start + 1000 ? 1 : 0)
    const latency = host.latencyFrames()
    const first = 1000 + latency + 2 * STEP16, second = first + 2 * STEP16
    assert.ok(peak(right, second - 50, second + 50) > 0.05, "a repeat")
    assert.ok(peak(left, first - 50, first + 50) < 0.01 && peak(right, first - 50, first + 50) > 0.05, "panned right")
    assert.equal(host.text(3, 63), "R63")
    assert.equal(host.text(3, -12), "L12")
    assert.equal(host.parse(3, "L12"), -12)
    assert.equal(host.text(1, 1), "1/8 T")
})

check("effects: the state keeps their parameters", () => {
    for (const kind of [KIND.DIST, KIND.PCF, KIND.COMP, KIND.DELAY]) {
        const host = new Host(kind)
        host.setParam(1, 1)
        const saved = host.save()
        const other = new Host(kind)
        assert.ok(other.load(saved))
        assert.equal(other.value(1), 1)
        assert.deepEqual(other.save(), saved)
    }
})

check("effects: a tail of their latency and ring, the delay's repeats until -60 dB, endless at full feedback", () => {
    for (const kind of [KIND.DIST, KIND.PCF, KIND.COMP]) {
        const host = new Host(kind)
        assert.ok(host.tailFrames() > host.latencyFrames(), `kind ${kind}`)
    }
    const host = new Host(KIND.DELAY)
    host.setParam(0, 4)
    host.setParam(2, pct(64))
    const quarter = SAMPLE_RATE * 60 / 120
    const tail = host.tailFrames()
    assert.ok(tail > quarter * 9 && tail < quarter * 13, `tail ${tail}, about 10 repeats of a beat`)
    host.setParam(2, 100)
    assert.equal(host.tailFrames(), 0xFFFFFFFF)
    for (const kind of [KIND.BASS, KIND.TR808, KIND.TR909]) {assert.equal(new Host(kind).tail, 0, "instruments: no tail")}
})

check("sleep in silence: the engine rests without notes or input, a note or input wakes it at once, the delay's late repeat comes", () => {
    for (const kind of [KIND.BASS, KIND.TR808, KIND.TR909]) {
        const host = new Host(kind, true)
        host.run([], SAMPLE_RATE)
        const [left] = host.run([on(host.position + 1000, 36)], 8000)
        assert.ok(peak(left, 1000, 4000) > 0.005, `kind ${kind} wakes on a note`)
        assert.ok(peak(left, 0, 1000) === 0, "silent before")
    }
    const host = new Host(KIND.DELAY)
    host.setParam(0, 16)
    host.setParam(2, 0)
    const start = host.position
    const [left] = host.run([], SAMPLE_RATE * 3, frame => frame === start + 1000 ? 1 : 0)
    const at = 1000 + host.latencyFrames() + 16 * STEP16
    assert.ok(peak(left, at - 50, at + 50) > 0.05, "a repeat after a second of silence")
    const comp = new Host(KIND.COMP)
    comp.run([], SAMPLE_RATE)
    const [out] = comp.run([], 4000, sine(220, 0.5))
    assert.ok(peak(out, 1000) > 0.1, "input wakes an effect")
})

check("speed: microseconds per 128 frames at 48 kHz, sounding and asleep", () => {
    const report = []
    const time = (host, blocks, events = () => [], input = undefined) => {
        const started = process.hrtime.bigint()
        for (let b = 0; b < blocks; b++) {host.run(events(b), 128, input)}
        return Number(process.hrtime.bigint() - started) / 1000 / blocks
    }
    for (const [kind, name] of [[KIND.BASS, "303"], [KIND.TR808, "808"], [KIND.TR909, "909"]]) {
        const playing = new Host(kind, false)
        playing.start(0)
        const busy = time(playing, 400)
        const idle = time(new Host(kind, true), 400)
        report.push(`${name} ${busy.toFixed(1)} / ${idle.toFixed(1)}`)
        assert.ok(busy < 1000, `${name}: ${busy} µs`)
    }
    for (const [kind, name] of [[KIND.DIST, "dist"], [KIND.PCF, "pcf"], [KIND.COMP, "comp"], [KIND.DELAY, "delay"]]) {
        const busy = time(new Host(kind), 400, () => [], sine(220, 0.5))
        const idle = time(new Host(kind), 400)
        report.push(`${name} ${busy.toFixed(1)} / ${idle.toFixed(1)}`)
        assert.ok(busy < 1000, `${name}: ${busy} µs`)
    }
    console.log(`    (sounding / asleep, µs per block, the host's buffers included) ${report.join(", ")}`)
})

check("the page's widths are the plugin's (page/index.html WIDTHS, the width features)", () => {
    const page = readFileSync(new URL("page/index.html", import.meta.url), "utf8")
    const widths = Function(`return ${page.match(/const WIDTHS = (\{[^}]*\})/)[1]}`)()
    const names = ["bass", "distortion", "pcf", "compressor", "delay", "tr808", "tr909"]
    names.forEach((name, kind) => {
        const host = new Host(kind)
        const feature = host.features.find(f => f.startsWith("opendaw:device-panel-width:"))
        assert.equal(Number(feature.split(":").pop()), widths[name], name)
    })
})

check("distortion: every curve fits the screen's fixed scale (page/index.html DISTORTION_SCALE)", () => {
    const page = readFileSync(new URL("page/index.html", import.meta.url), "utf8")
    const scale = Number(page.match(/const DISTORTION_SCALE = ([0-9.]+)/)[1])
    let highest = 0
    const host = new Host(KIND.DIST)
    for (let amount = 0; amount <= 127; amount += 7) {
        for (let shape = 0; shape <= 32; shape++) {
            host.setParam(0, pct(amount))
            host.setParam(1, shape)
            const status = host.receive(new Uint8Array([PAGE.POLL])).sent[0]
            const view = new DataView(status.buffer, status.byteOffset)
            for (let i = 0; i < 65; i++) {highest = Math.max(highest, Math.abs(view.getFloat32(8 + 16 + i * 4, true)))}
        }
    }
    console.log(`    (the loudest curve ${highest.toFixed(3)})`)
    assert.ok(highest <= scale, `${highest} > ${scale}`)
})

console.log(`${passed} checks passed`)
