// Hive in node: a minimal CLAP host with a transport runs the sequencer and checks the note stream (timing, swing,
// gate, triggers, pattern changes on the bar, stop and jumps, repeatability), then the state layout and the page traffic.
// Usage: node check.mjs <module.wasm>
import assert from "node:assert/strict"
import {readFileSync} from "node:fs"

const SAMPLE_RATE = 48000
const BLOCK = 128
const EVENT = {NOTE_ON: 0, NOTE_OFF: 1, NOTE_CHOKE: 2, PARAM_VALUE: 5, GESTURE_BEGIN: 7, GESTURE_END: 8, MIDI: 10}
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
    static modules = new Map()
    // compiled once and instantiated synchronously: with stdout to a file, node (23) waited forever on an async
    // instantiate after a few instances
    static async load(path) {
        const module = Host.modules.get(path) ?? new WebAssembly.Module(readFileSync(path))
        Host.modules.set(path, module)
        const imports = {}
        for (const entry of WebAssembly.Module.imports(module)) {
            (imports[entry.module] ??= {})[entry.name] = () => {throw new Error(`${entry.module}.${entry.name} called`)}
        }
        const instance = new WebAssembly.Instance(module, imports)
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
        this.playing = false
        this.tempo = 120
        this.beatBase = 0
        this.baseAt = 0
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
        const strings = ["hive check", "open-webclap", "", "1"].map(text => this.string(text))
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
            view.setUint32(this.process + 12, this.writeTransport(), true)
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
    // the host's transport per block: playing, tempo, the beat position (beatBase plus the samples since baseAt)
    writeTransport() {
        this.transportPtr ??= this.malloc(104)
        const view = this.view, pointer = this.transportPtr
        this.bytes.fill(0, pointer, pointer + 104)
        view.setUint32(pointer, 104, true)
        view.setUint16(pointer + 10, 9, true)
        view.setUint32(pointer + 16, (this.playing ? 16 : 0) | 1 | 2 | 8, true)
        const beats = this.beatBase + (this.position - this.baseAt) / SAMPLE_RATE * this.tempo / 60 + (this.jitter?.(this.position / BLOCK) ?? 0)
        view.setBigInt64(pointer + 24, BigInt(Math.round(beats * 2 ** 31)), true)
        view.setFloat64(pointer + 40, this.tempo, true)
        view.setUint16(pointer + 100, 4, true)
        view.setUint16(pointer + 102, 4, true)
        return pointer
    }
    locate(beats) {this.beatBase = beats; this.baseAt = this.position}
}

const PARAM = {PATTERN: 0, GATE: 1, TRANSPOSE: 2, RATE: 3}
// the shared messages (opendaw_page.h), then Hive's own
const PAGE = {POLL: 1, BEGIN: 2, SET: 3, END: 4, HOVER: 7, HELLO: 8, SHOW_WINDOW: 9, TILE: 10, HEADER: 11, PATTERN: 12, COMMIT: 13}
const TILE_COUNT = 169, PATTERN_BYTES = 8 + TILE_COUNT * 16, STATE_BYTES = 16 + 8 * PATTERN_BYTES
const BAR = SAMPLE_RATE * 2 // 4 beats at 120 bpm
const STEP = BAR / 16       // 1/16 = 6000 samples
const on = (time, key, velocity = 0.8) => ({time, type: EVENT.NOTE_ON, key, velocity, channel: 0})
const off = (time, key) => ({time, type: EVENT.NOTE_OFF, key, velocity: 0, channel: 0})
const ons = notes => notes.filter(note => note.on)
const brief = notes => notes.map(({time, on, key}) => `${on ? "+" : "-"}${key}@${time}`)

// per key: on and off alternate, every note ends
const wellFormed = (notes, label) => {
    const sounding = new Map()
    for (const note of notes) {
        const down = sounding.get(note.key) ?? false
        assert.equal(down, !note.on, `${label}: ${note.on ? "on" : "off"} of ${note.key} at ${note.time} while ${down ? "sounding" : "silent"}`)
        sounding.set(note.key, note.on)
    }
    for (let i = 1; i < notes.length; i++) {assert.ok(notes[i].time >= notes[i - 1].time, `${label}: sorted`)}
}

let passed = 0
const check = async (name, test) => {
    const host = await Host.load(process.argv[2])
    await test(host)
    passed++
    console.log(`ok  ${name}`)
}
const play = (host, bars, events = []) => {
    host.playing = true
    host.run(events, host.position + Math.round(bars * BAR))
}
const cellIndex = (() => {
    const map = new Map()
    let index = 0
    for (let q = -7; q <= 7; q++) {for (let r = Math.max(-7, -q - 7); r <= Math.min(7, -q + 7); r++) {map.set(q + "," + r, index++)}}
    return (q, r) => map.get(q + "," + r)
})()
const tileMessage = (pattern, index, bytes, live = false) => {
    const message = new Uint8Array(24)
    message[0] = PAGE.TILE; message[1] = live ? 1 : 0; message[2] = pattern
    new DataView(message.buffer).setUint16(4, index, true)
    message.set(bytes, 8)
    return message
}

// the test pattern (the first factory pattern of 0.1): a ring of notes fed from the centre (its first note at
// step 2), a runner from the left edge with a ratchet ×3 at step 3 (it also starts on notes), a split, a sink, a turn
const fixture = host => {
    const tiles = Array.from({length: TILE_COUNT}, () => [0, 0, 255, 1, 34, 255, 100, 1, 100, 0, 2, 0, 16, 255, 2, 0])
    const at = (q, r) => tiles[cellIndex(q, r)]
    const set = (q, r, values) => Object.entries(values).forEach(([index, value]) => at(q, r)[index] = value)
    const KIND = 0, FLAGS = 1, DIR = 2, TURN = 3, FORK = 4, VELOCITY = 6, REPEAT = 7, TRIGGER = 9, RATE = 10, OFFSET = 11, LIFE = 12
    set(0, 0, {[KIND]: 1, [DIR]: 0, [RATE]: 2, [LIFE]: 20})
    set(1, 0, {[DIR]: 0})
    const DQ = [1, 1, 0, -1, -1, 0], DR = [0, -1, -1, 0, 1, 1]
    let q = DQ[4] * 2, r = DR[4] * 2
    for (let side = 0; side < 6; side++) {
        for (let step = 0; step < 2; step++) {
            set(q, r, {[DIR]: side, [FLAGS]: 1})
            q += DQ[side]; r += DR[side]
        }
    }
    set(-4, 2, {[KIND]: 1, [DIR]: 0, [RATE]: 1, [OFFSET]: 2, [LIFE]: 7, [TRIGGER]: 2})
    set(-3, 2, {[FLAGS]: 1, [REPEAT]: 3, [VELOCITY]: 80})
    set(0, -2, {[KIND]: 4, [FORK]: (1 << 3) | (1 << 2)})
    set(3, -1, {[KIND]: 2})
    set(2, -3, {[KIND]: 3, [TURN]: 2, [FLAGS]: 1})
    const message = new Uint8Array(8 + PATTERN_BYTES)
    message[0] = PAGE.PATTERN
    message.set([5, 0, 48, 5, 0, 0, 0, 0], 8)
    tiles.forEach((tile, index) => message.set(tile, 16 + index * 16))
    host.receive(message)
    return host
}
const fixtureHost = async () => fixture(await Host.load(process.argv[2]))

await check("descriptor: a note effect with openDAW's device panel", async host => {
    assert.equal(host.id, "studio.opendaw.hive")
    assert.ok(host.features.includes("note-effect") && host.features.includes("opendaw:device-panel")
        && host.features.includes("opendaw:device-panel-width:266"))
    assert.ok(host.panel && host.webview && host.state && host.params)
})

await check("silent while the transport stops", async host => {
    host.run([], BAR * 2)
    assert.equal(host.take().length, 0)
})

await check("notes on their sample: the ring at step 2, the runner's ratchet ×3 at step 3", async host => {
    fixture(host)
    play(host, 1)
    const notes = host.take()
    wellFormed(notes, "bar")
    const times = ons(notes).map(note => note.time)
    assert.equal(times[0], 2 * STEP, brief(notes).slice(0, 4).join(" "))
    for (const t of [3 * STEP, 3 * STEP + STEP / 3, 3 * STEP + 2 * STEP / 3]) {assert.ok(times.includes(t), `ratchet at ${t}`)}
    const first = notes.find(note => note.on)
    const end = notes.find(note => !note.on && note.key === first.key)
    assert.equal(end.time - first.time, Math.floor(STEP * 0.8), "gate 80 % of the step")
})

await check("a tile's duration: a cursor stays a 1/4 on it, the ring's first note comes three steps later", async host => {
    fixture(host)
    const index = cellIndex(1, 0) // between the centre spawner and the ring, east, no note
    host.receive(tileMessage(0, index, [0, 0, 0, 1, 34, 255, 100, 1, 100, 0, 2, 0, 16, 255, 6, 0]))
    play(host, 1)
    const notes = host.take()
    wellFormed(notes, "bar")
    const times = ons(notes).map(note => note.time)
    assert.ok(!times.includes(2 * STEP), "not at step 2")
    assert.ok(times.includes(5 * STEP), "at step 5 (1 + 4)")
    assert.ok(times.includes(3 * STEP), "the runner is not held up")
})

await check("rate 2/1: cursors walk twice as fast, the ring's first note at step 1", async host => {
    fixture(host)
    host.setParam(PARAM.RATE, 4)
    host.take()
    host.locate(0)
    const start = host.position
    play(host, 0.5)
    const times = ons(host.take()).map(note => note.time - start)
    assert.equal(times[0], STEP, `first at ${times[0]}`)
})

await check("a groove before us (ranges shifted by up to 0.05 beats) neither skips nor repeats a note", async host => {
    fixture(host)
    play(host, 4)
    const straight = ons(host.take()).map(note => note.key)
    const grooved = await fixtureHost()
    grooved.jitter = block => [0, 0.05, -0.03, 0.02, -0.05][block % 5]
    play(grooved, 4)
    const notes = grooved.take()
    wellFormed(notes, "grooved")
    // the last block may run up to 0.05 beats ahead, a step past the straight run's end
    const keys = ons(notes).map(note => note.key)
    assert.deepEqual(keys.slice(0, straight.length), straight)
    assert.ok(keys.length - straight.length <= 4, `${keys.length - straight.length} extra at the end`)
})

await check("a note in starts cursors on note spawners, transposed by its key", async host => {
    fixture(host)
    play(host, 0.25)
    host.take()
    const at = host.position + 100
    play(host, 0.5, [on(at, 72, 1.0), off(at + 200, 72)])
    const notes = ons(host.take())
    const plain = new Set()
    const reference = await fixtureHost()
    play(reference, 0.75)
    ons(reference.take()).forEach(note => plain.add(`${note.key}@${note.time}`))
    const extra = notes.filter(note => !plain.has(`${note.key}@${note.time}`))
    assert.ok(extra.length > 0, "the note added notes")
    assert.ok(extra.every(note => note.time >= at), "none before the note")
})

await check("a pattern change waits for the bar while playing", async host => {
    host.receive(new Uint8Array([PAGE.HELLO]))
    play(host, 0.5)
    host.setParam(PARAM.PATTERN, 1)
    let delta = host.receive(new Uint8Array([PAGE.POLL])).sent.at(-1)
    assert.equal(delta[2], 0, "still the first pattern")
    play(host, 0.6)
    delta = host.receive(new Uint8Array([PAGE.POLL])).sent.at(-1)
    assert.equal(delta[2], 1, "the second after the bar")
})

await check("stop ends what sounds, a jump starts over", async host => {
    play(host, 0.3)
    host.playing = false
    host.run([], host.position + 256)
    wellFormed(host.take(), "stop")
    play(host, 0.3)
    host.locate(32)
    play(host, 0.7)
    wellFormed(host.take(), "jump")
})

await check("repeatable: the random walker plays the same twice", async host => {
    host.setParam(PARAM.PATTERN, 1)
    host.take()
    const relative = from => host.take().map(({time, on, key}) => `${on ? "+" : "-"}${key}@${time - from}`)
    host.locate(0)
    let start = host.position
    play(host, 4)
    const first = relative(start)
    host.reset()
    host.playing = false
    host.run([], host.position + 256)
    host.locate(0)
    start = host.position
    play(host, 4)
    const second = relative(start)
    assert.ok(first.length > 20, "it plays")
    assert.deepEqual(second, first)
})

await check("state: fixed layout, a tile edit changes 16 bytes, live edits wait for the commit", async host => {
    const before = host.save()
    assert.equal(before.length, STATE_BYTES)
    assert.equal(String.fromCharCode(...before.slice(0, 4)), "HIVE")
    const index = cellIndex(1, 1)
    const tile = [0, 1, 2, 0, 0, 60, 90, 2, 100, 0, 2, 0, 16, 255, 2, 0]
    host.dirty = 0
    host.receive(tileMessage(3, index, tile, true))
    assert.equal(host.dirty, 0, "live: not yet")
    host.receive(new Uint8Array([PAGE.COMMIT]))
    assert.equal(host.dirty, 1, "the commit")
    host.receive(tileMessage(3, index, tile))
    assert.equal(host.dirty, 2, "not live: at once")
    const after = host.save()
    const changed = [...after].map((byte, i) => byte !== before[i] ? i : -1).filter(i => i >= 0)
    assert.ok(changed.length > 0 && changed.length <= 16, `${changed.length} bytes`)
    assert.ok(changed.every(i => i >= 16 + 3 * PATTERN_BYTES + 8 + index * 16 && i < 16 + 3 * PATTERN_BYTES + 8 + index * 16 + 16))
    const other = await Host.load(process.argv[2])
    assert.ok(other.load(after), "load")
    assert.deepEqual(other.save(), after, "round trip")
    const broken = after.slice()
    broken.fill(0xff, 16, 16 + PATTERN_BYTES)
    assert.ok(other.load(broken), "garbage loads")
    const sane = other.save()
    assert.equal(sane[16], 8, "size clamped")
})

await check("the factory pattern plays from the first beat", async host => {
    play(host, 2)
    const notes = host.take()
    wellFormed(notes, "factory")
    assert.equal(ons(notes)[0].time, 0, "the bass on the first beat")
    assert.ok(ons(notes).length > 40, `${ons(notes).length} notes in two bars`)
})

await check("page: a snapshot on hello, nothing while idle, edits and cursors as deltas", async host => {
    const hello = host.receive(new Uint8Array([PAGE.HELLO]))
    assert.equal(hello.sent.length, 1)
    assert.equal(hello.sent[0].length, 8 + 4 * 8 + 8 * PATTERN_BYTES)
    assert.equal(host.receive(new Uint8Array([PAGE.POLL])).sent.length, 0, "idle: nothing")
    host.receive(tileMessage(0, cellIndex(0, 1), [0, 1, 0, 1, 0, 255, 100, 1, 100, 0, 2, 0, 16, 255, 2, 0]))
    const edit = host.receive(new Uint8Array([PAGE.POLL])).sent[0]
    assert.equal(new DataView(edit.buffer).getUint16(6, true), 1, "one edit")
    assert.equal(edit.length, 8 + 20 + 22, "header, the edit, the tiles played")
    play(host, 0.5)
    const playing = host.receive(new Uint8Array([PAGE.POLL])).sent[0]
    assert.equal(playing[3] & 1, 1, "playing")
    assert.equal(playing.length, 8 + 22 + playing[4] * 4 + playing[5] * 4, "cursors and edge hits")
    assert.ok(playing[4] > 0, "cursors")
    play(host, 4)
    const hits = host.receive(new Uint8Array([PAGE.POLL])).sent[0]
    assert.ok(hits[5] > 0, "cursors reach the edge within four bars")
    console.log(`    snapshot ${hello.sent[0].length} bytes, tile edit ${edit.length}, playing ${playing.length} (${playing[4]} cursors)`)
})

console.log(`hive: all ${passed} checks passed`)
