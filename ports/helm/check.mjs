// Helm in node: a minimal CLAP host (after ports/note-lab/check.mjs) plays notes and checks the audio (timing,
// short notes, reset, sleep), the state layout (bytes per edit), a patch load (one gesture) and the page traffic,
// then measures the DSP. Usage: node check.mjs <module.wasm>

import assert from "node:assert/strict"
import {readFileSync} from "node:fs"

const SAMPLE_RATE = 48000
const BLOCK = 128
const EVENT = {NOTE_ON: 0, NOTE_OFF: 1, PARAM_VALUE: 5, GESTURE_BEGIN: 7, GESTURE_END: 8, MIDI: 10}
const PAGE = {HELLO: 1, POLL: 2, BEGIN: 3, SET: 4, END: 5, HOVER: 6, MODULATE: 7, PATCH: 8}
const PLUGIN = {DESCRIBE: 0x81, SNAPSHOT: 0x82, VALUES: 0x83, ACTIVITY: 0x84}
const STATUS = {CONTINUE: 1, SLEEP: 4}
const STATE_SIZE = 884, VALUES_AT = 8, SLOTS_AT = 8 + 139 * 4, NAME_AT = SLOTS_AT + 32 * 8
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
        const imports = {}
        for (const entry of WebAssembly.Module.imports(module)) {
            (imports[entry.module] ??= {})[entry.name] = () => 0
        }
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
        this.position = 0
        const entry = exports.clap_entry.value
        assert.ok(this.fn(entry + 12)(this.string("/")), "entry init")
        const factory = this.fn(entry + 20)(this.string("clap.plugin-factory"))
        const descriptor = this.fn(factory + 4)(factory, 0)
        this.id = this.cstr(this.u32(descriptor + 12))
        this.createHost()
        this.plugin = this.fn(factory + 8)(factory, this.host, this.string(this.id))
        assert.ok(this.plugin, "create")
        const started = performance.now()
        assert.ok(this.fn(this.plugin + 8)(this.plugin), "init")
        this.initMillis = performance.now() - started
        const extension = name => this.fn(this.plugin + 40)(this.plugin, this.string(name))
        this.params = extension("clap.params")
        this.state = extension("clap.state")
        this.webview = extension("clap.webview/3")
        this.gui = extension("clap.gui")
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
        const strings = ["helm check", "open-webclap", "", "1"].map(text => this.string(text))
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
        this.install(params + 8, ["i32"], [], () => this.flushes++)
        const state = extension("clap.state", 4)
        this.install(state, ["i32"], [], () => this.dirty++)
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
        // one stereo output (clap_audio_buffer: data32, data64, channel_count, latency, constant_mask)
        this.channels = [this.malloc(4 * 1024), this.malloc(4 * 1024)]
        this.channelPointers = this.malloc(8)
        this.view.setUint32(this.channelPointers, this.channels[0], true)
        this.view.setUint32(this.channelPointers + 4, this.channels[1], true)
        this.audioOut = this.malloc(24)
        this.bytes.fill(0, this.audioOut, this.audioOut + 24)
        this.view.setUint32(this.audioOut, this.channelPointers, true)
        this.view.setUint32(this.audioOut + 8, 2, true)
        this.scratch = this.malloc(4096)
        this.count = 0
    }

    read(pointer) {
        const view = this.view
        const type = view.getUint16(pointer + 10, true)
        if (type === EVENT.PARAM_VALUE) {
            this.out.push({type, param: view.getUint32(pointer + 16, true), value: view.getFloat64(pointer + 40, true)})
        } else if (type === EVENT.GESTURE_BEGIN || type === EVENT.GESTURE_END) {
            this.out.push({type, param: view.getUint32(pointer + 16, true)})
        }
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
            this.position = end
        }
        return {left, status}
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
    pageSet(id, value) {
        this.page(PAGE.BEGIN, view => view.setUint16(2, id, true), 4)
        this.page(PAGE.SET, view => {
            view.setUint16(2, id, true)
            view.setFloat32(4, value, true)
        })
        this.page(PAGE.END, view => view.setUint16(2, id, true), 4)
    }
    modulate(source, destination, amount, live = false) {
        return this.page(PAGE.MODULATE, view => {
            view.setUint8(1, source)
            view.setUint8(2, destination)
            view.setUint8(3, live ? 1 : 0)
            view.setFloat32(4, amount, true)
        })
    }
    flush() {
        this.write([])
        this.fn(this.params + 20)(this.plugin, this.inEvents, this.outEvents)
    }
    reset() {this.fn(this.plugin + 32)(this.plugin)}
}

const on = (time, key, velocity = 0.8) => ({time, type: EVENT.NOTE_ON, key, velocity})
const off = (time, key) => ({time, type: EVENT.NOTE_OFF, key})
const firstSound = samples => samples.findIndex(sample => Math.abs(sample) > 1e-6)
const rms = samples => Math.sqrt(samples.reduce((sum, sample) => sum + sample * sample, 0) / Math.max(1, samples.length))
const changedBytes = (a, b) => a.reduce((count, byte, index) => count + (byte !== b[index] ? 1 : 0), 0)

const path = process.argv[2]
assert.ok(path, "usage: node check.mjs <module.wasm>")
const host = await Host.load(path)
console.log(`init ${host.initMillis.toFixed(1)} ms`)

// the page's table
const hello = host.page(PAGE.HELLO, () => {}, 4)
assert.equal(hello.sent[0][0], PLUGIN.DESCRIBE)
const description = JSON.parse(new TextDecoder().decode(hello.sent[0].subarray(4)))
assert.equal(description.params.length, 139)
const id = name => description.params.findIndex(param => param.name === name)
const source = name => description.sources.indexOf(name)
assert.equal(hello.sent[1][0], PLUGIN.SNAPSHOT)
assert.equal(hello.sent[1].length, STATE_SIZE)
console.log(`hello: description ${hello.sent[0].length} bytes, snapshot ${hello.sent[1].length} bytes`)

// the window resizes at its aspect ratio, from half to twice the size (openDAW's zoom levels need can_resize)
{
    const size = host.malloc(20)
    const gui = (offset, ...args) => host.fn(host.gui + offset)(host.plugin, ...args)
    assert.ok(gui(24), "can_resize")
    assert.ok(gui(28, size))
    assert.deepEqual([host.view.getUint8(size + 2), host.u32(size + 4), host.u32(size + 8)], [1, 1128, 700], "aspect ratio")
    host.view.setUint32(size, 1692, true)
    host.view.setUint32(size + 4, 2000, true)
    gui(32, size, size + 4)
    assert.deepEqual([host.u32(size), host.u32(size + 4)], [1692, 1050], "adjusted to 150 %")
    assert.ok(gui(36, 1692, 1050), "set_size")
    assert.ok(!gui(36, 1692, 900), "another aspect ratio is refused")
    gui(20, size, size + 4)
    assert.deepEqual([host.u32(size), host.u32(size + 4)], [1692, 1050], "get_size")
    assert.ok(gui(36, 1128, 700))
    console.log("window resizes at 1128:700")
}

// notes start at their sample
let result = host.run([on(77, 60)], 4 * BLOCK)
assert.ok(firstSound(result.left) >= 77 && firstSound(result.left) < 77 + 8, `note at 77 sounds from ${firstSound(result.left)}`)
host.run([off(host.position, 60)], host.position + SAMPLE_RATE)
console.log(`note at sample 77 sounds from ${firstSound(result.left)}`)

// a note on and off inside one block still sounds (the block splits at the second event of the key)
let start = host.position
result = host.run([on(start + 10, 64), off(start + 60, 64)], start + 8 * BLOCK)
assert.ok(rms(result.left) > 1e-4, "a short note sounds")
host.run([], host.position + SAMPLE_RATE)

// reset: silent at once
start = host.position
host.run([on(start, 48), on(start, 55)], start + 20 * BLOCK)
host.reset()
result = host.run([], host.position + 4 * BLOCK)
assert.equal(firstSound(result.left), -1, "silent after reset")

// asleep after half a second of silence, awake with the next note
result = host.run([], host.position + SAMPLE_RATE)
assert.equal(result.status, STATUS.SLEEP, "sleeps")
start = host.position
result = host.run([on(start + 5, 60)], start + 2 * BLOCK)
assert.ok(firstSound(result.left) >= 5, "wakes with the note")
host.run([off(host.position, 60)], host.position + SAMPLE_RATE)

// the state: fixed size, a knob edit changes at most 4 bytes, a routing at most 8, its amount at most 4
const initial = host.save()
assert.equal(initial.length, STATE_SIZE)
host.pageSet(id("cutoff"), 90)
host.flush()
let saved = host.save()
assert.ok(changedBytes(initial, saved) <= 4, "knob edit")
assert.equal(new DataView(saved.buffer).getFloat32(VALUES_AT + id("cutoff") * 4, true), 90)
const values = host.out.filter(event => event.type === EVENT.PARAM_VALUE)
assert.deepEqual(values.map(({param, value}) => [param, value]), [[id("cutoff"), 90]])
host.out = []

let before = saved
host.dirty = 0
assert.ok(host.modulate(source("mono_lfo_1"), id("cutoff"), 12.0, true).handled)
assert.equal(host.dirty, 0, "a live amount does not mark the state")
assert.ok(host.modulate(source("mono_lfo_1"), id("cutoff"), 24.0).handled)
assert.equal(host.dirty, 1, "the drag's end marks the state")
saved = host.save()
assert.ok(changedBytes(before, saved) <= 8, "routing added")
before = saved
assert.ok(host.modulate(source("mono_lfo_1"), id("cutoff"), 20.0).handled)
saved = host.save()
assert.ok(changedBytes(before, saved) <= 4, "amount changed")
assert.ok(!host.modulate(source("mono_lfo_1"), id("polyphony") === -1 ? 0 : id("legato"), 1.0).handled, "legato is no destination")

// round trip: the state loads, saves the same bytes, the page gets a snapshot
assert.ok(host.load(initial))
assert.deepEqual(host.save(), initial)
assert.ok(host.load(saved))
assert.deepEqual(host.save(), saved)
let poll = host.page(PAGE.POLL, () => {}, 4)
assert.equal(poll.sent[0][0], PLUGIN.SNAPSHOT)

// a patch from the page: one gesture around every value that changed, the routings in the state
const patch = new Uint8Array(STATE_SIZE)
patch.set(initial)
const patchView = new DataView(patch.buffer)
patch[0] = PAGE.PATCH
patchView.setFloat32(VALUES_AT + id("osc_1_waveform") * 4, 2, true)
patchView.setFloat32(VALUES_AT + id("resonance") * 4, 0.8, true)
patch.set([source("fil_envelope"), id("cutoff"), 0, 0], SLOTS_AT)
patchView.setFloat32(SLOTS_AT + 4, 30, true)
patch.set(new TextEncoder().encode("Test Patch"), NAME_AT)
host.out = []
host.dirty = 0
assert.ok(host.receive(patch).handled)
host.flush()
const kinds = host.out.map(event => event.type)
const begins = kinds.filter(kind => kind === EVENT.GESTURE_BEGIN).length
assert.ok(begins >= 2 && kinds.lastIndexOf(EVENT.GESTURE_BEGIN) < kinds.indexOf(EVENT.PARAM_VALUE)
    && kinds.lastIndexOf(EVENT.PARAM_VALUE) < kinds.indexOf(EVENT.GESTURE_END), "one gesture")
assert.equal(host.dirty, 1)
saved = host.save()
assert.equal(new TextDecoder().decode(saved.subarray(NAME_AT, NAME_AT + 10)), "Test Patch")
console.log(`patch load: ${begins} values in one gesture`)
host.out = []
poll = host.page(PAGE.POLL, () => {}, 4)
assert.equal(poll.sent[0][0], PLUGIN.SNAPSHOT, "the page gets the patch as the plugin took it")

// page traffic: a host change is 12 bytes, nothing while nothing changes
host.run([{time: host.position, type: EVENT.PARAM_VALUE, id: id("volume"), value: 0.5}], host.position + BLOCK)
poll = host.page(PAGE.POLL, () => {}, 4)
const valueMessages = poll.sent.filter(message => message[0] === PLUGIN.VALUES)
assert.equal(valueMessages.length, 1)
assert.equal(valueMessages[0].length, 12)
host.run([], host.position + SAMPLE_RATE)
host.page(PAGE.POLL, () => {}, 4)
host.run([], host.position + 4 * BLOCK)
poll = host.page(PAGE.POLL, () => {}, 4)
assert.equal(poll.sent.length, 0, "nothing while nothing sounds")
start = host.position
host.run([on(start, 60)], start + 10 * BLOCK)
poll = host.page(PAGE.POLL, () => {}, 4)
assert.ok(poll.sent.every(message => message[0] === PLUGIN.ACTIVITY && message.length === 4 + 28), "activity is 32 bytes")
host.run([off(host.position, 60)], host.position + SAMPLE_RATE)
console.log("state and page traffic ok")

// a patch that switches the arpeggiator on, loaded while the engine sleeps: the next note plays at once (Helm
// drops the held notes when the arpeggiator switches, the note must come after that)
host.run([], host.position + SAMPLE_RATE)
const arp = new Uint8Array(saved)
arp[0] = PAGE.PATCH
new DataView(arp.buffer).setFloat32(VALUES_AT + id("arp_on") * 4, 1, true)
assert.ok(host.receive(arp).handled)
start = host.position
result = host.run([on(start, 60)], start + SAMPLE_RATE / 10)
assert.ok(rms(result.left) > 1e-3, "an arpeggio right after the patch")
host.run([off(host.position, 60)], host.position + SAMPLE_RATE)
host.receive(patch)
host.flush()
host.out = []
console.log("patch then note: sounds at once")

// DSP: 8 voices, both oscillators with 7 unison voices, filter, distortion, delay and reverb on
for (const [name, value] of [["osc_1_unison_voices", 7], ["osc_2_unison_voices", 7], ["filter_on", 1],
    ["distortion_on", 1], ["delay_on", 1], ["reverb_on", 1], ["polyphony", 8]]) {
    host.run([{time: host.position, type: EVENT.PARAM_VALUE, id: id(name), value}], host.position + BLOCK)
}
for (const block of [128, 32]) {
    start = host.position
    const notes = [0, 3, 7, 10, 12, 15, 19, 22].map(step => on(start, 48 + step))
    const began = performance.now()
    host.run(notes, start + 10 * SAMPLE_RATE, block)
    const millis = performance.now() - began
    console.log(`8 voices x 2 x 7 unison, effects, blocks of ${block}: ${(millis / 100).toFixed(2)} % of real time`)
    host.reset()
}
console.log("all checks passed")
