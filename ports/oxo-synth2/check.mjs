// Oxo Synth2 in node: a minimal CLAP host (after ports/helm/check.mjs) plays notes and checks the audio (timing,
// short notes, reset, sleep), the parameters and their texts, the state layout (bytes per edit, the macOS 'OXS2'
// chunk import), a patch load from the page (one gesture) and the page traffic, then measures the DSP.
// Usage: node check.mjs <module.wasm>

import assert from "node:assert/strict"
import {readFileSync} from "node:fs"

const SAMPLE_RATE = 48000
const BLOCK = 128
const EVENT = {NOTE_ON: 0, NOTE_OFF: 1, NOTE_CHOKE: 2, PARAM_VALUE: 5, MIDI: 10, GESTURE_BEGIN: 7, GESTURE_END: 8}
const PAGE = {HELLO: 1, POLL: 2, BEGIN: 3, SET: 4, END: 5, HOVER: 6, PATCH: 8, INIT: 9, PANIC: 10}
const PLUGIN = {DESCRIBE: 0x81, SNAPSHOT: 0x82, VALUES: 0x83, ACTIVITY: 0x84}
const STATUS = {CONTINUE: 1, SLEEP: 4}
const PARAMS = 100 // 0..98 the patch, 99 the volume
const STATE_SIZE = 472, SNAPSHOT_SIZE = 4 + STATE_SIZE - 8
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
    static async load(path) {
        const module = await WebAssembly.compile(readFileSync(path))
        for (const entry of WebAssembly.Module.imports(module))
            assert.equal(entry.module, "wasi_snapshot_preview1", `import ${entry.module}.${entry.name} is not WASI`)
        const imports = {}
        for (const entry of WebAssembly.Module.imports(module))
            (imports[entry.module] ??= {})[entry.name] = () => 0
        const instance = await WebAssembly.instantiate(module, imports)
        instance.exports._initialize?.()
        return new Host(instance.exports)
    }

    constructor(exports) {
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
        assert.equal(this.id, "com.oxo.music.oxosynth2")
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
        assert.ok(this.fn(this.plugin + 16)(this.plugin, SAMPLE_RATE, 1, BLOCK), "activate")
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
        const strings = ["oxo check", "open-webclap", "", "1"].map(text => this.string(text))
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
                view.setUint32(pointer, 32, true)
                this.bytes.set(event.data, pointer + 16)
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

    // absolute sample times, in blocks of `block`; returns the left channel and the last status
    run(events, until, block = BLOCK) {
        const queue = events.toSorted((first, second) => first.time - second.time)
        const left = []
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
            this.count = 0
            this.out = []
            this.position = end
        }
        return {left, status}
    }

    // process in a tight loop, without the host filling buffers, for the timing
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
    pageDrag(id, value) {
        this.page(PAGE.BEGIN, view => view.setUint32(4, id, true))
        this.page(PAGE.SET, view => {
            view.setUint32(4, id, true)
            view.setFloat64(8, value, true)
        }, 16)
        this.page(PAGE.END, view => view.setUint32(4, id, true))
    }
    flush() {
        this.write([])
        this.out = []
        this.fn(this.params + 20)(this.plugin, this.inEvents, this.outEvents)
        return this.out
    }
    reset() {this.fn(this.plugin + 32)(this.plugin)}
}

const on = (time, key, velocity = 0.8) => ({time, type: EVENT.NOTE_ON, key, velocity})
const off = (time, key) => ({time, type: EVENT.NOTE_OFF, key})
const param = (time, id, value) => ({time, type: EVENT.PARAM_VALUE, id, value})
const midi = (time, data) => ({time, type: EVENT.MIDI, data})
const firstSound = samples => samples.findIndex(sample => Math.abs(sample) > 1e-6)
const rms = samples => Math.sqrt(samples.reduce((sum, sample) => sum + sample * sample, 0) / Math.max(1, samples.length))
const changedBytes = (a, b) => a.reduce((count, byte, index) => count + (byte !== b[index] ? 1 : 0), 0)
const FLAG = {STEPPED: 1, HIDDEN: 4, AUTOMATABLE: 32, ENUM: 1 << 16}

const path = process.argv[2]
assert.ok(path, "usage: node check.mjs <module.wasm>")
const host = await Host.load(path)
console.log(`loaded: "${host.name}" ${host.version}, id ${host.id}, WASI imports only`)

// the descriptor's features
{
    const list = []
    for (let pointer = host.u32(host.descriptor + 44); host.u32(pointer) !== 0; pointer += 4)
        list.push(host.cstr(host.u32(pointer)))
    assert.ok(list.includes("instrument") && list.includes("synthesizer") && list.includes("stereo"), "features")
    console.log(`features: ${list.join(", ")}`)
}

// the tail is unbounded (the delay line rings)
assert.equal(host.fn(host.tail)(host.plugin) >>> 0, 0xffffffff, "tail")

// the window resizes at 770:560, from half to twice the size
{
    const size = host.malloc(20)
    const gui = (offset, ...args) => host.fn(host.gui + offset)(host.plugin, ...args)
    assert.ok(gui(24), "can_resize")
    assert.ok(gui(28, size))
    assert.deepEqual([host.view.getUint8(size + 2), host.u32(size + 4), host.u32(size + 8)], [1, 770, 560], "aspect ratio")
    host.view.setUint32(size, 3080, true)
    host.view.setUint32(size + 4, 2240, true)
    gui(32, size, size + 4)
    assert.deepEqual([host.u32(size), host.u32(size + 4)], [1540, 1120], "adjusted to 200 %")
    assert.ok(gui(36, 1540, 1120), "set_size")
    assert.ok(!gui(36, 1540, 900), "another aspect ratio is refused")
    gui(20, size, size + 4)
    assert.deepEqual([host.u32(size), host.u32(size + 4)], [1540, 1120], "get_size")
    console.log("window resizes at 770:560, 0.5..2x")
}

// parameters: 100, native values, sections, texts both ways
assert.equal(host.fn(host.params)(host.plugin), PARAMS, "param count")
{
    const osc1 = host.paramInfo(0)
    assert.deepEqual([osc1.id, osc1.min, osc1.max, osc1.def, osc1.module], [0, 0, 3, 2, "Oscillators"])
    assert.ok(osc1.flags & FLAG.STEPPED && osc1.flags & FLAG.ENUM && osc1.flags & FLAG.AUTOMATABLE, "osc1 flags")
    const freq = host.paramInfo(19)
    assert.deepEqual([freq.id, freq.min, freq.max, freq.def, freq.module], [19, 0, 127, 32, "Filter"])
    assert.ok(!(freq.flags & FLAG.ENUM), "freq not enum")
    const src = host.paramInfo(86)
    assert.ok(src.flags & FLAG.HIDDEN && !(src.flags & FLAG.AUTOMATABLE), "ctrl src hidden")
    assert.equal(src.min, -1)
    const volume = host.paramInfo(99)
    assert.deepEqual([volume.id, volume.min, volume.max, volume.def, volume.module], [99, 0, 127, 64, "Master"])
    assert.equal(host.valueToText(0, 2), "pulse")
    assert.equal(host.valueToText(1, 4), "noise")
    assert.equal(host.valueToText(14, 1), "LP24")
    assert.equal(host.valueToText(31, 4), "random")
    assert.equal(host.valueToText(33, 11), "1/8")
    assert.equal(host.valueToText(99, 64), "+0.0 dB")
    assert.equal(host.valueToText(99, 0), "-inf dB")
    assert.equal(host.textToValue(14, "LP24"), 1)
    assert.equal(host.textToValue(19, "100"), 100)
    assert.equal(host.paramValue(19), 32, "default value")
    console.log("parameters: 100 with native ranges, texts both ways")
}

// the page's hello: the table (JSON) and a snapshot, then nothing while nothing changes
const hello = host.page(PAGE.HELLO, () => {}, 4)
assert.equal(hello.sent[0][0], PLUGIN.DESCRIBE)
const description = JSON.parse(new TextDecoder().decode(hello.sent[0].subarray(4)))
assert.equal(description.params.length, PARAMS)
assert.equal(description.params[0].labels.join(","), "sine,saw,pulse,tri")
assert.equal(description.params[42].labels.length, 6)
assert.ok(description.params[43].syncLabels, "lfo speed sync labels")
assert.equal(description.init.length, 99)
assert.equal(description.effectKnobNames.length, 10)
assert.equal(hello.sent[1][0], PLUGIN.SNAPSHOT)
assert.equal(hello.sent[1].length, SNAPSHOT_SIZE)
const idle = host.page(PAGE.POLL)
assert.ok(idle.sent.length === 0 || idle.sent.every(message => message[0] === PLUGIN.ACTIVITY), "idle poll sends at most the activity")
const idle2 = host.page(PAGE.POLL)
assert.equal(idle2.sent.length, 0, "nothing while nothing does")
console.log(`hello: description ${hello.sent[0].length} bytes, snapshot ${hello.sent[1].length} bytes`)

// a page drag is one gesture to the host: begin, value, end, and the state marked dirty once
{
    const before = host.dirty
    host.pageDrag(19, 100)
    const out = host.flush()
    assert.deepEqual(out.map(event => event.type), [EVENT.GESTURE_BEGIN, EVENT.PARAM_VALUE, EVENT.GESTURE_END], "one gesture")
    assert.equal(out[1].param, 19)
    assert.equal(out[1].value, 100)
    assert.equal(host.paramValue(19), 100, "the value took")
    assert.equal(host.dirty, before + 1, "dirty at the drag's end")
    assert.ok(host.flushes > 0, "flush requested")
    host.page(PAGE.POLL) // drain the drag's own value from the page's queue
    // the page hears of host-side changes: a PARAM_VALUE event, then a poll answers with the value
    host.run([param(host.position, 20, 90)], host.position + BLOCK)
    const values = host.page(PAGE.POLL)
    const message = values.sent.find(m => m[0] === PLUGIN.VALUES)
    assert.ok(message, "values answer")
    const view = new DataView(message.buffer ?? message)
    assert.equal(view.getUint16(2, true), 1)
    assert.equal(view.getUint32(4, true), 20)
    assert.equal(view.getFloat64(12, true), 90)
    assert.equal(host.paramValue(20), 90)
    console.log("page drag = one gesture, host changes answered on poll")
}

// hover reaches the host
host.page(PAGE.HOVER, view => view.setUint32(4, 19, true))
assert.equal(host.hovered, 19)
host.page(PAGE.HOVER, view => view.setUint32(4, 0xffffffff, true))
assert.equal(host.hovered >>> 0, 0xffffffff)

// state: 472 bytes fixed, an edit changes 4 bytes, round trip, and the macOS 'OXS2' chunk loads
{
    const saved = host.save()
    assert.equal(saved.length, STATE_SIZE, "state size")
    assert.deepEqual([...saved.subarray(0, 4)], [..."OS2W"].map(c => c.charCodeAt(0)), "state magic")
    host.run([param(host.position, 19, 101)], host.position + BLOCK)
    const edited = host.save()
    const changed = changedBytes(saved, edited)
    assert.ok(changed > 0 && changed <= 4, `one knob edit changes at most 4 bytes (${changed})`)
    assert.ok(host.load(saved), "load own state")
    host.run([], host.position + SAMPLE_RATE) // the patch-switch fade lands the values at silence
    assert.equal(host.paramValue(19), 100, "the load restored the value")
    assert.deepEqual(host.save(), saved, "round trip is byte identical")

    // 'OXS2' v2 chunk: magic + version 2 + volume + .sy1 text
    const lines = ["TESTPATCH", "color=default", "ver=112"]
    for (let i = 0; i < 99; i++) {lines.push(`${i},${i === 19 ? 111 : i === 0 ? 2 : 64}`)} // 64 clamps into the small ranges
    const text = new TextEncoder().encode(lines.join("\n") + "\n")
    const chunk = new Uint8Array(8 + 4 + text.length)
    chunk.set([..."OXS2"].map(c => c.charCodeAt(0)), 0)
    new DataView(chunk.buffer).setUint32(4, 2, true)
    new DataView(chunk.buffer).setUint32(8, 64, true)
    chunk.set(text, 12)
    assert.ok(host.load(chunk), "legacy OXS2 chunk loads")
    host.run([], host.position + SAMPLE_RATE)
    assert.equal(host.paramValue(19), 111, "legacy value applied")
    assert.equal(host.paramValue(0), 2)
    console.log("state: 472 bytes, 4 bytes per edit, round trip, OXS2 import")
}

// notes snap to the 16-sample grid and sound; a short note in one block sounds; reset silences; sleep and wake
{
    host.run([], host.position + BLOCK) // settle
    const now = host.position
    let result = host.run([on(now + 77, 60)], now + 4 * BLOCK)
    const first = firstSound(result.left)
    assert.ok(first >= 64 && first <= 80, `note at 77 sounds from ${first} (snapped to 64)`)
    console.log(`note at 77 sounds from sample ${first} (16-sample grid)`)
    host.run([off(host.position, 60)], host.position + SAMPLE_RATE)

    const start = host.position
    result = host.run([on(start + 10, 64), off(start + 60, 64)], start + 8 * BLOCK)
    assert.ok(rms(result.left) > 1e-4, "a short note sounds")

    // MIDI dialect: note on/off and CC 64 (sustain) through CLAP_EVENT_MIDI
    host.run([midi(host.position, [0x90, 60, 100])], host.position + 2 * BLOCK)
    result = host.run([midi(host.position, [0xb0, 64, 127]), midi(host.position + BLOCK / 2, [0x80, 60, 0])],
                      host.position + 2 * BLOCK)
    assert.ok(rms(result.left) > 1e-5, "sustain holds the note after note off")
    host.run([midi(host.position, [0xb0, 64, 0]), midi(host.position, [0xb0, 123, 0])], host.position + SAMPLE_RATE)

    // reset silences at once
    host.run([on(host.position, 48), on(host.position, 55)], host.position + 4 * BLOCK)
    host.reset()
    result = host.run([], host.position + 2 * BLOCK)
    assert.equal(firstSound(result.left), -1, "silent after reset")

    // sleeps in silence, wakes on a note
    let status = 0, until = host.position + 5 * SAMPLE_RATE
    while (host.position < until && status !== STATUS.SLEEP)
        status = host.run([], host.position + BLOCK).status
    assert.equal(status, STATUS.SLEEP, "sleeps after half a second of silence")
    result = host.run([on(host.position + 8, 60)], host.position + BLOCK)
    assert.ok(firstSound(result.left) >= 0, "wakes on a note")
    host.run([off(host.position, 60)], host.position + 2 * SAMPLE_RATE)
    console.log("reset, sleep in silence, wake on a note")
}

// a patch from the page: applied, one gesture around the changed values, the snapshot carries the name
{
    const values = new Int32Array(99).fill(0)
    values[19] = 120
    values[0] = 3
    const name = "WEBPATCH"
    const encoded = new TextEncoder().encode(name)
    const message = new Uint8Array(4 + encoded.length + 99 * 4)
    const view = new DataView(message.buffer)
    view.setUint8(0, PAGE.PATCH)
    view.setUint16(2, encoded.length, true)
    message.set(encoded, 4)
    for (let i = 0; i < 99; i++) {view.setInt32(4 + encoded.length + 4 * i, values[i], true)}
    const before = host.dirty
    const result = host.receive(message)
    assert.ok(result.handled, "patch handled")
    const out = host.flush()
    assert.ok(out.some(event => event.type === EVENT.GESTURE_BEGIN), "gesture begins")
    const firstValue = out.findIndex(event => event.type === EVENT.PARAM_VALUE)
    const lastBegin = out.map((event, index) => event.type === EVENT.GESTURE_BEGIN ? index : -1).filter(index => index >= 0).pop()
    const firstEnd = out.findIndex(event => event.type === EVENT.GESTURE_END)
    assert.ok(lastBegin < firstValue && firstValue < firstEnd, "one gesture around all values")
    host.run([], host.position + SAMPLE_RATE) // the patch-switch fade lands the values
    assert.equal(host.paramValue(19), 120)
    assert.equal(host.paramValue(0), 3)
    assert.equal(host.dirty, before + 1, "one dirty mark")
    const poll = host.page(PAGE.POLL)
    const snapshot = poll.sent.find(m => m[0] === PLUGIN.SNAPSHOT)
    assert.ok(snapshot, "snapshot after a patch load")
    assert.equal(new TextDecoder().decode(snapshot.subarray(4 + 396 + 4, 4 + 396 + 4 + 8)), "WEBPATCH", "patch name")
    console.log("page patch: applied, one gesture, name in the snapshot")
}

// INIT from the page restores the init patch
{
    host.page(PAGE.INIT, () => {}, 4)
    host.flush()
    host.run([], host.position + SAMPLE_RATE)
    assert.equal(host.paramValue(19), description.init[19], "init patch applied")
    console.log("INIT patch from the page")
}

// the DSP: a chord with unison, timed per block
{
    host.run([param(host.position, 73, 1), param(host.position, 93, 8)], host.position + BLOCK) // unison on, 8 voices
    const chord = [36, 48, 60, 64, 67, 72, 79, 84].map(key => on(host.position, key, 0.9))
    host.run(chord, host.position + 4 * BLOCK)
    const blocks = 300
    const started = performance.now()
    const millis = host.time(blocks)
    const micros = millis * 1000
    console.log(`dsp: ${micros.toFixed(1)} us per ${BLOCK}-frame block with an 8-note unison chord ` +
                `(${(micros / (BLOCK / SAMPLE_RATE * 1e6) * 100).toFixed(1)} % of real time at ${SAMPLE_RATE} Hz)`)
    assert.ok(micros < BLOCK / SAMPLE_RATE * 1e6 * 0.5, "well under real time")
    host.run([off(host.position, 36), off(host.position, 48), off(host.position, 60), off(host.position, 64),
              off(host.position, 67), off(host.position, 72), off(host.position, 79), off(host.position, 84)],
             host.position + SAMPLE_RATE)
}

console.log("all checks passed")
