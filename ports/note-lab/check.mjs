// Note Lab in node: a minimal CLAP host feeds notes, reads the note stream back and checks it per mode, then the
// state layout (bytes per edit) and the page traffic (bytes per message). Usage: node check.mjs <module.wasm>

import assert from "node:assert/strict"
import {readFileSync} from "node:fs"

const SAMPLE_RATE = 48000
const BLOCK = 128
const EVENT = {NOTE_ON: 0, NOTE_OFF: 1, NOTE_CHOKE: 2, PARAM_VALUE: 5, GESTURE_BEGIN: 7, GESTURE_END: 8, MIDI: 10}
const PARAM = {PRESET: 0, MODE: 1, DELAY: 2, REPEATS: 3, TRANSPOSE: 4}
const MODE = {THRU: 0, HARMONY: 1, ECHO: 2, HOLD: 3}
const PAGE = {HELLO: 1, POLL: 2, BEGIN: 3, SET: 4, END: 5, HOVER: 6, SHOW_WINDOW: 7}
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
            (imports[entry.module] ??= {})[entry.name] = () => {throw new Error(`${entry.module}.${entry.name} called`)}
        }
        const instance = await WebAssembly.instantiate(module, imports)
        instance.exports._initialize?.()
        return new Host(instance.exports)
    }

    constructor(exports) {
        this.memory = exports.memory
        this.table = exports.__indirect_function_table
        this.malloc = exports.malloc
        this.events = []
        this.out = []
        this.sent = []
        this.flushes = 0
        this.dirty = 0
        this.shown = 0
        this.position = 0
        const entry = exports.clap_entry.value
        assert.ok(this.fn(entry + 12)(this.string("/")), "entry init")
        const factory = this.fn(entry + 20)(this.string("clap.plugin-factory"))
        const descriptor = this.fn(factory + 4)(factory, 0)
        this.id = this.cstr(this.u32(descriptor + 12))
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
        this.panel = extension("opendaw.device-panel/1")
        this.createProcess()
        assert.ok(this.fn(this.plugin + 16)(this.plugin, SAMPLE_RATE, 1, BLOCK), "activate")
        assert.ok(this.fn(this.plugin + 24)(this.plugin), "start_processing")
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
        const strings = ["note-lab check", "open-webclap", "", "1"].map(text => this.string(text))
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
        this.scratch = this.malloc(4096)
        this.count = 0
    }

    read(pointer) {
        const view = this.view
        const time = this.position + view.getUint32(pointer + 4, true)
        const type = view.getUint16(pointer + 10, true)
        if (type === EVENT.NOTE_ON || type === EVENT.NOTE_OFF) {
            this.out.push({time, on: type === EVENT.NOTE_ON, channel: view.getInt16(pointer + 22, true),
                key: view.getInt16(pointer + 24, true), velocity: view.getFloat64(pointer + 32, true)})
        } else if (type === EVENT.PARAM_VALUE) {
            this.out.push({time, param: view.getUint32(pointer + 16, true), value: view.getFloat64(pointer + 40, true)})
        } else if (type === EVENT.MIDI) {
            this.out.push({time, midi: [...this.bytes.slice(pointer + 18, pointer + 21)]})
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
                view.setInt32(pointer + 24, -1, true)
                view.setFloat64(pointer + 40, event.value, true)
            } else if (event.type === EVENT.MIDI) {
                view.setUint32(pointer, 24, true)
                event.data.forEach((byte, offset) => view.setUint8(pointer + 18 + offset, byte))
            } else {
                view.setUint32(pointer, 40, true)
                view.setInt32(pointer + 16, -1, true)
                view.setInt16(pointer + 22, event.channel ?? 0, true)
                view.setInt16(pointer + 24, event.key, true)
                view.setFloat64(pointer + 32, event.velocity ?? 0.8, true)
            }
        })
        this.count = events.length
    }

    // absolute sample times, split into blocks like a host does
    run(events, until) {
        const queue = events.toSorted((first, second) => first.time - second.time)
        assert.ok(queue.every(event => event.time >= this.position), "no event before the current position")
        while (this.position < until) {
            const end = this.position + BLOCK
            const block = []
            while (queue.length > 0 && queue[0].time < end) {
                const event = queue.shift()
                block.push({...event, time: event.time - this.position})
            }
            this.write(block)
            const view = this.view
            view.setBigUint64(this.process, BigInt(this.position), true)
            view.setUint32(this.process + 8, BLOCK, true)
            view.setUint32(this.process + 12, 0, true)
            for (const offset of [16, 20, 24, 28]) {view.setUint32(this.process + offset, 0, true)}
            view.setUint32(this.process + 32, this.inEvents, true)
            view.setUint32(this.process + 36, this.outEvents, true)
            assert.equal(this.fn(this.plugin + 36)(this.plugin, this.process), 1, "process continues")
            this.count = 0
            this.position = end
        }
    }

    notes() {return this.out.filter(event => event.key !== undefined)}
    take() {
        const notes = this.notes()
        this.out = []
        return notes
    }
    setParam(id, value) {this.run([{time: this.position, type: EVENT.PARAM_VALUE, id, value}], this.position + BLOCK)}
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
    pageParam(type, id, value) {
        const view = new DataView(new ArrayBuffer(type === PAGE.SET ? 16 : 8))
        view.setUint8(0, type)
        view.setUint32(4, id, true)
        if (type === PAGE.SET) {view.setFloat64(8, value, true)}
        return this.receive(new Uint8Array(view.buffer))
    }
    reset() {this.fn(this.plugin + 32)(this.plugin)}
}

const on = (time, key, velocity = 0.8, channel = 0) => ({time, type: EVENT.NOTE_ON, key, velocity, channel})
const off = (time, key, channel = 0) => ({time, type: EVENT.NOTE_OFF, key, velocity: 0, channel})
const brief = notes => notes.map(({time, on, key}) => `${on ? "+" : "-"}${key}@${time}`)

// per key: sorted times, on and off alternate, nothing left sounding unless allowed
const wellFormed = (notes, allowHeld = false) => {
    let last = -1
    const sounding = new Set()
    for (const note of notes) {
        assert.ok(note.time >= last, `sorted at ${note.time}`)
        last = note.time
        const key = `${note.channel}:${note.key}`
        assert.equal(sounding.has(key), !note.on, `${note.on ? "double on" : "off without on"} ${key} at ${note.time}`)
        if (note.on) {sounding.add(key)} else {sounding.delete(key)}
    }
    if (!allowHeld) {assert.equal(sounding.size, 0, `left sounding: ${[...sounding]}`)}
    return sounding
}

const tests = []
const test = (name, body) => tests.push({name, body})
const fresh = async () => Host.load(process.argv[2])

test("describes a note effect with a device panel and no audio", async () => {
    const host = await fresh()
    assert.equal(host.id, "studio.opendaw.note-lab")
    assert.ok(host.features.includes("note-effect"))
    assert.ok(host.features.includes("opendaw:device-panel"))
    assert.equal(host.fn(host.audioPorts)(host.plugin, 1), 0)
    assert.equal(host.fn(host.audioPorts)(host.plugin, 0), 0)
    assert.equal(host.fn(host.notePorts)(host.plugin, 1), 1)
    assert.equal(host.fn(host.notePorts)(host.plugin, 0), 1)
    const width = host.malloc(4)
    assert.ok(host.fn(host.panel + 4)(host.plugin, 176, width))
    assert.equal(host.u32(width), 352)
    const uri = host.malloc(64)
    host.fn(host.panel)(host.plugin, uri, 64)
    assert.equal(host.cstr(uri), "/index.html?panel")
})

test("thru keeps every note at its sample", async () => {
    const host = await fresh()
    host.run([on(10, 60), on(130, 64, 0.5), off(255, 60), off(256, 64)], 1024)
    assert.deepEqual(brief(host.take()), ["+60@10", "+64@130", "-60@255", "-64@256"])
})

test("thru reads the MIDI dialect and passes other MIDI messages", async () => {
    const host = await fresh()
    host.run([{time: 5, type: EVENT.MIDI, data: [0x91, 62, 127]}, {time: 7, type: EVENT.MIDI, data: [0xb1, 64, 127]},
        {time: 300, type: EVENT.MIDI, data: [0x81, 62, 0]}], 512)
    assert.deepEqual(host.out.filter(event => event.midi !== undefined), [{time: 7, midi: [0xb1, 64, 127]}])
    const notes = host.take()
    assert.deepEqual(brief(notes), ["+62@5", "-62@300"])
    assert.equal(notes[0].channel, 1)
    assert.equal(notes[0].velocity, 1)
})

test("harmony adds the third and the fifth at the same sample", async () => {
    const host = await fresh()
    host.setParam(PARAM.MODE, MODE.HARMONY)
    host.take()
    host.run([on(1000, 60), off(2000, 60)], 2560)
    assert.deepEqual(brief(host.take()), ["+60@1000", "+64@1000", "+67@1000", "-60@2000", "-64@2000", "-67@2000"])
})

test("harmony re-attacks a shared key and releases it with the last source", async () => {
    const host = await fresh()
    host.setParam(PARAM.MODE, MODE.HARMONY)
    host.take()
    host.run([on(1000, 60), on(1100, 64), off(1200, 60), off(1300, 64)], 1536)
    const notes = host.take()
    wellFormed(notes)
    assert.deepEqual(brief(notes.filter(note => note.key === 64)), ["+64@1000", "-64@1100", "+64@1100", "-64@1300"])
})

test("echo repeats at the exact delay, across blocks, with falling velocity", async () => {
    const host = await fresh()
    host.setParam(PARAM.MODE, MODE.ECHO)
    host.setParam(PARAM.DELAY, 250)
    host.setParam(PARAM.REPEATS, 3)
    host.take()
    const start = 1000, delay = 12000
    host.run([on(start, 60, 0.8), off(start + 100, 60)], start + 4 * delay)
    const notes = host.take()
    wellFormed(notes)
    assert.deepEqual(notes.filter(note => note.on).map(note => note.time), [0, 1, 2, 3].map(repeat => start + repeat * delay))
    assert.deepEqual(notes.filter(note => !note.on).map(note => note.time), [0, 1, 2, 3].map(repeat => start + 100 + repeat * delay))
    assert.deepEqual(notes.filter(note => note.on).map(note => Math.round(note.velocity * 100)), [80, 60, 40, 20])
})

test("echo keeps the delay and repeats a note started with", async () => {
    const host = await fresh()
    host.setParam(PARAM.MODE, MODE.ECHO)
    host.setParam(PARAM.REPEATS, 1)
    host.take()
    host.run([on(1000, 60)], 1100)
    host.setParam(PARAM.DELAY, 1000)
    host.setParam(PARAM.REPEATS, 5)
    host.run([off(2000, 60)], 30000)
    const notes = host.take()
    wellFormed(notes)
    assert.deepEqual(brief(notes), ["+60@1000", "-60@2000", "+60@13000", "-60@14000"])
})

test("hold drops note offs and ends the held notes with the next note", async () => {
    const host = await fresh()
    host.setParam(PARAM.MODE, MODE.HOLD)
    host.take()
    host.run([on(1100, 60), on(1100, 64), off(1200, 60), off(1200, 64), on(1500, 67), off(1600, 67)], 2048)
    assert.deepEqual(brief(host.take()), ["+60@1100", "+64@1100", "-60@1500", "-64@1500", "+67@1500"])
})

test("a transpose while a note sounds still releases the key it started", async () => {
    const host = await fresh()
    host.run([on(10, 60)], 128)
    host.setParam(PARAM.TRANSPOSE, 7)
    host.run([off(300, 60), on(400, 60), off(500, 60)], 640)
    assert.deepEqual(brief(host.take()), ["+60@10", "-60@300", "+67@400", "-67@500"])
})

test("transposed out of range is dropped, never clamped", async () => {
    const host = await fresh()
    host.setParam(PARAM.TRANSPOSE, 24)
    host.run([on(1010, 120), off(1020, 120), on(1030, 100), off(1040, 100)], 1152)
    assert.deepEqual(brief(host.take()), ["+124@1030", "-124@1040"])
})

test("a mode change releases every sounding note at its sample", async () => {
    const host = await fresh()
    host.setParam(PARAM.MODE, MODE.HOLD)
    host.run([on(200, 60), off(210, 60)], 384)
    host.run([{time: 400, type: EVENT.PARAM_VALUE, id: PARAM.MODE, value: MODE.THRU}], 512)
    const notes = host.take()
    wellFormed(notes)
    assert.deepEqual(brief(notes), ["+60@200", "-60@400"])
})

test("a mode change from the page releases with the next block", async () => {
    const host = await fresh()
    host.setParam(PARAM.MODE, MODE.HOLD)
    host.run([on(200, 60), off(210, 60)], 384)
    host.pageParam(PAGE.SET, PARAM.MODE, MODE.ECHO)
    host.run([], 512)
    const notes = host.take()
    wellFormed(notes)
    assert.deepEqual(brief(notes), ["+60@200", "-60@384"])
})

test("CC 123 releases everything", async () => {
    const host = await fresh()
    host.setParam(PARAM.MODE, MODE.ECHO)
    host.take()
    host.run([on(1010, 60), {time: 1050, type: EVENT.MIDI, data: [0xb0, 123, 0]}], 48000)
    const notes = host.take()
    wellFormed(notes)
    assert.deepEqual(brief(notes), ["+60@1010", "-60@1050"])
})

test("a preset pick reports the values it brings", async () => {
    const host = await fresh()
    host.run([{time: 0, type: EVENT.PARAM_VALUE, id: PARAM.PRESET, value: 7}], 128)
    const values = Object.fromEntries(host.out.filter(event => event.param !== undefined).map(event => [event.param, event.value]))
    assert.deepEqual(values, {[PARAM.MODE]: MODE.ECHO, [PARAM.DELAY]: 500, [PARAM.REPEATS]: 2, [PARAM.TRANSPOSE]: 7})
})

test("random streams stay well formed in every mode", async () => {
    let seed = 7
    const random = () => (seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0) / 4294967296
    for (const mode of [MODE.THRU, MODE.HARMONY, MODE.ECHO, MODE.HOLD]) {
        const host = await fresh()
        host.setParam(PARAM.MODE, mode)
        host.setParam(PARAM.DELAY, 37)
        host.take()
        const events = []
        const down = new Set()
        for (let time = 1000; time < 40000; time += 1 + Math.floor(random() * 300)) {
            const key = 48 + Math.floor(random() * 24)
            const channel = Math.floor(random() * 2)
            const id = `${channel}:${key}`
            if (down.has(id) && random() < 0.7) {
                events.push(off(time, key, channel))
                down.delete(id)
            } else {
                events.push(on(time, key, random(), channel))
                down.add(id)
            }
        }
        down.forEach(id => {
            const [channel, key] = id.split(":").map(Number)
            events.push(off(40000, key, channel))
        })
        host.run(events, 80000)
        const sounding = wellFormed(host.take(), mode === MODE.HOLD)
        assert.ok(mode === MODE.HOLD || sounding.size === 0)
    }
})

test("the state is 72 bytes and an edit changes at most four of them", async () => {
    const host = await fresh()
    const before = host.save()
    assert.equal(before.length, 72)
    const changed = (first, second) => first.reduce((count, byte, index) => count + (byte !== second[index] ? 1 : 0), 0)
    host.setParam(PARAM.DELAY, 333)
    const delay = host.save()
    assert.ok(changed(before, delay) <= 4, `delay changed ${changed(before, delay)} bytes`)
    host.setParam(PARAM.REPEATS, 5)
    assert.equal(changed(delay, host.save()), 1)
    const repeats = host.save()
    host.setParam(PARAM.PRESET, 3)
    assert.equal(changed(repeats, host.save()), 1)
    const other = await fresh()
    assert.ok(other.load(host.save()))
    assert.deepEqual(other.save(), host.save())
    assert.ok(!other.load(new Uint8Array(72)))
})

test("the page gets a snapshot on hello, then only what changed", async () => {
    const host = await fresh()
    const hello = host.receive(Uint8Array.of(PAGE.HELLO))
    assert.equal(hello.sent.length, 1)
    assert.equal(hello.sent[0][0], 1)
    assert.equal(hello.sent[0].length, 80)
    assert.equal(host.receive(Uint8Array.of(PAGE.POLL)).sent.length, 0, "nothing changed, nothing sent")
    host.run([on(10, 60), off(20, 60)], 128)
    const delta = host.receive(Uint8Array.of(PAGE.POLL)).sent
    assert.equal(delta.length, 1)
    assert.equal(delta[0][0], 2)
    assert.equal(delta[0].length, 4 + 4 * 4, "two in and two out events, four bytes each")
    host.setParam(PARAM.TRANSPOSE, 3)
    const value = host.receive(Uint8Array.of(PAGE.POLL)).sent
    assert.equal(value[0].length, 4 + 8, "one value")
    assert.equal(value[0][1], 1 << PARAM.TRANSPOSE)
})

test("a page edit marks the state dirty, asks for a flush and reports the value", async () => {
    const host = await fresh()
    host.pageParam(PAGE.BEGIN, PARAM.DELAY)
    host.pageParam(PAGE.SET, PARAM.DELAY, 400)
    host.pageParam(PAGE.END, PARAM.DELAY)
    assert.equal(host.dirty, 1)
    assert.ok(host.flushes >= 1)
    host.run([], 128)
    assert.deepEqual(host.out.filter(event => event.param !== undefined), [{time: 0, param: PARAM.DELAY, value: 400}])
})

test("the page opens the big window through the host", async () => {
    const host = await fresh()
    assert.equal(host.receive(Uint8Array.of(PAGE.SHOW_WINDOW)).handled, 1)
    assert.equal(host.shown, 1)
})

test("reset forgets, nothing sounds afterwards", async () => {
    const host = await fresh()
    host.setParam(PARAM.MODE, MODE.ECHO)
    host.run([on(1010, 60)], 1280)
    host.reset()
    host.take()
    host.run([off(48000, 60)], 96000)
    assert.deepEqual(host.take(), [])
})

let failed = 0
for (const {name, body} of tests) {
    try {
        await body()
        console.log(`ok   ${name}`)
    } catch (error) {
        failed++
        console.log(`FAIL ${name}\n     ${error.message}`)
    }
}
console.log(failed === 0 ? `note-lab: all ${tests.length} checks passed` : `note-lab: ${failed} of ${tests.length} checks failed`)
process.exit(failed === 0 ? 0 : 1)
