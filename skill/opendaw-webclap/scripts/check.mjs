// Checks a built plugin against what openDAW needs (docs/building-for-opendaw.md), in node, without a browser: a
// small CLAP host loads builds/<slug>.wclap.tar.gz and tests the descriptor, the device panel, parameters and their
// texts, the state, the page's messages, rendering at any block size, reset, speed, and the page's files.
//
//     node scripts/check.mjs ./my-plugin
//
// ✗ is a failure (exit code 1), ! a warning worth fixing, ✓ passed.
import fs from "node:fs"
import path from "node:path"
import {CLAP_ID_PREFIX, archiveOf, fail, pluginFolder, unpack} from "./lib.mjs"

const {folder, slug, meta} = pluginFolder(process.argv[2])
const archive = archiveOf(folder, slug)
if (!fs.existsSync(archive)) {fail(`${archive} does not exist. Build first: node scripts/build.mjs ${process.argv[2]}`)}
const files = unpack(new Uint8Array(fs.readFileSync(archive)))
const fileOf = name => files.find(file => file.path === name)
const wasm = fileOf("module.wasm")
if (wasm === undefined) {fail("The bundle holds no module.wasm")}

const results = []
const pass = name => results.push({kind: "pass", name})
const failure = (name, reason) => results.push({kind: "fail", name, reason})
const warning = (name, reason) => results.push({kind: "warn", name, reason})
const expect = (condition, name, reason) => condition ? pass(name) : failure(name, reason)

const SAMPLE_RATE = 48000
const BLOCK = 128
const EVENT = {NOTE_ON: 0, NOTE_OFF: 1, NOTE_CHOKE: 2, PARAM_VALUE: 5, GESTURE_BEGIN: 7, GESTURE_END: 8, TRANSPORT: 9, MIDI: 10}
const PAGE = {POLL: 1, BEGIN: 2, SET: 3, END: 4, HOVER: 7, DESCRIBE: 8}
const VALTYPE = {i32: 0x7f, i64: 0x7e, f32: 0x7d, f64: 0x7c}
const FLAGS = {AUTOMATABLE: 1 << 5, STEPPED: 1 << 0, ENUM: 1 << 16}

// a JS function as a wasm function reference, to put host callbacks into the plugin's table
const trampoline = (params, results, fn) => {
    const name = text => [text.length, ...[...text].map(letter => letter.charCodeAt(0))]
    const section = (id, bytes) => [id, bytes.length, ...bytes]
    const type = [1, 0x60, params.length, ...params.map(kind => VALTYPE[kind]), results.length, ...results.map(kind => VALTYPE[kind])]
    const bytes = new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        ...section(1, type), ...section(2, [1, ...name("e"), ...name("f"), 0x00, 0x00]), ...section(7, [1, ...name("f"), 0x00, 0x00])])
    return new WebAssembly.Instance(new WebAssembly.Module(bytes), {e: {f: fn}}).exports.f
}

// minimal WASI: what a standalone Emscripten module may call
const wasi = memoryRef => ({
    fd_write: (fd, iovs, count, written) => {
        const view = new DataView(memoryRef.memory.buffer)
        let total = 0, text = ""
        for (let index = 0; index < count; index++) {
            const pointer = view.getUint32(iovs + index * 8, true), length = view.getUint32(iovs + index * 8 + 4, true)
            text += new TextDecoder().decode(new Uint8Array(memoryRef.memory.buffer, pointer, length))
            total += length
        }
        if (text.trim().length > 0) {memoryRef.log.push(text.trim())}
        view.setUint32(written, total, true)
        return 0
    },
    fd_close: () => 0, fd_seek: () => 0, fd_read: () => 0, fd_fdstat_get: () => 0, fd_prestat_get: () => 8,
    fd_prestat_dir_name: () => 8, environ_sizes_get: (count, size) => {
        const view = new DataView(memoryRef.memory.buffer)
        view.setUint32(count, 0, true)
        view.setUint32(size, 0, true)
        return 0
    },
    environ_get: () => 0, args_sizes_get: (count, size) => {
        const view = new DataView(memoryRef.memory.buffer)
        view.setUint32(count, 0, true)
        view.setUint32(size, 0, true)
        return 0
    },
    args_get: () => 0, proc_exit: code => {throw new Error(`proc_exit(${code})`)},
    clock_time_get: (id, precision, time) => {
        new DataView(memoryRef.memory.buffer).setBigUint64(time, BigInt(Math.round(performance.now() * 1e6)), true)
        return 0
    },
    random_get: (pointer, length) => {
        crypto.getRandomValues(new Uint8Array(memoryRef.memory.buffer, pointer, length))
        return 0
    }
})

class Host {
    static async load(bytes) {
        const module = await WebAssembly.compile(bytes)
        const memoryRef = {memory: null, log: []}
        const imports = {}
        const shim = wasi(memoryRef)
        for (const entry of WebAssembly.Module.imports(module)) {
            const fn = entry.module === "wasi_snapshot_preview1" ? shim[entry.name] : undefined
            ;(imports[entry.module] ??= {})[entry.name] = fn ?? (() => {throw new Error(`${entry.module}.${entry.name} called`)})
        }
        const instance = await WebAssembly.instantiate(module, imports)
        memoryRef.memory = instance.exports.memory
        instance.exports._initialize?.()
        return new Host(instance.exports, memoryRef)
    }

    constructor(exports, memoryRef) {
        this.memoryRef = memoryRef
        this.memory = exports.memory
        this.table = exports.__indirect_function_table ?? Object.values(exports).find(value => value instanceof WebAssembly.Table)
        this.malloc = exports.malloc
        this.out = []
        this.sent = []
        this.flushes = 0
        this.dirty = 0
        this.hovered = []
        this.latencyChanged = 0
        this.showRequests = 0
        this.position = 0
        const entry = exports.clap_entry.value
        if (!this.fn(entry + 12)(this.string("/"))) {throw new Error("clap_entry.init failed")}
        this.factory = this.fn(entry + 20)(this.string("clap.plugin-factory"))
        if (this.factory === 0) {throw new Error("no plugin factory")}
        this.count = this.fn(this.factory)(this.factory)
        const descriptor = this.fn(this.factory + 4)(this.factory, 0)
        const string = offset => this.cstr(this.u32(descriptor + offset))
        this.descriptor = {id: string(12), name: string(16), vendor: string(20), version: string(36), description: string(40)}
        this.features = []
        for (let pointer = this.u32(descriptor + 44); this.u32(pointer) !== 0; pointer += 4) {this.features.push(this.cstr(this.u32(pointer)))}
        this.createHost()
        this.plugin = this.fn(this.factory + 8)(this.factory, this.host, this.string(this.descriptor.id))
        if (this.plugin === 0) {throw new Error("create_plugin returned null")}
        if (!this.fn(this.plugin + 8)(this.plugin)) {throw new Error("plugin.init failed")}
        const extension = name => this.fn(this.plugin + 40)(this.plugin, this.string(name))
        this.ext = {
            params: extension("clap.params"), state: extension("clap.state"), webview: extension("clap.webview/3"),
            gui: extension("clap.gui"), notePorts: extension("clap.note-ports"), audioPorts: extension("clap.audio-ports"),
            panel: extension("opendaw.device-panel/1"), latency: extension("clap.latency"), tail: extension("clap.tail")
        }
        this.createProcess()
    }

    get view() {return new DataView(this.memory.buffer)}
    get bytes() {return new Uint8Array(this.memory.buffer)}
    u32(pointer) {return this.view.getUint32(pointer, true)}
    fn(pointer) {return this.table.get(this.u32(pointer))}
    cstr(pointer) {
        if (pointer === 0) {return ""}
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
        const strings = ["opendaw-webclap check", "openDAW", "", "1"].map(text => this.string(text))
        this.view.setUint32(host, 1, true)
        this.view.setUint32(host + 4, 2, true)
        this.view.setUint32(host + 8, 0, true)
        this.view.setUint32(host + 12, 0, true)
        strings.forEach((pointer, index) => this.view.setUint32(host + 16 + index * 4, pointer, true))
        this.install(host + 32, ["i32", "i32"], ["i32"], (_host, id) => extensions.get(this.cstr(id)) ?? 0)
        for (const offset of [36, 40, 44]) {this.install(host + offset, ["i32"], [], () => {})}
        const params = extension("clap.params", 12)
        this.install(params, ["i32", "i32"], [], () => {})
        this.install(params + 4, ["i32", "i32", "i32"], [], () => {})
        this.install(params + 8, ["i32"], [], () => this.flushes++)
        const state = extension("clap.state", 4)
        this.install(state, ["i32"], [], () => this.dirty++)
        const gui = extension("clap.gui", 20)
        this.install(gui, ["i32"], [], () => {})
        this.install(gui + 4, ["i32", "i32", "i32"], ["i32"], () => 0)
        this.install(gui + 8, ["i32"], ["i32"], () => (this.showRequests++, 1))
        this.install(gui + 12, ["i32"], ["i32"], () => 0)
        this.install(gui + 16, ["i32", "i32"], [], () => {})
        const webview = extension("clap.webview/3", 4)
        this.install(webview, ["i32", "i32", "i32"], ["i32"], (_host, pointer, size) => {
            this.sent.push(this.bytes.slice(pointer, pointer + size))
            return 1
        })
        const hovered = extension("clap.param-hovered/1", 4)
        this.install(hovered, ["i32", "i32"], [], (_host, id) => this.hovered.push(id >>> 0))
        const latency = extension("clap.latency", 4)
        this.install(latency, ["i32"], [], () => this.latencyChanged++)
        const tail = extension("clap.tail", 4)
        this.install(tail, ["i32"], [], () => {})
        const log = extension("clap.log", 4)
        this.install(log, ["i32", "i32", "i32"], [], (_host, _severity, message) => this.memoryRef.log.push(this.cstr(message)))
        this.host = host
    }

    createProcess() {
        this.process = this.malloc(40)
        this.slots = this.malloc(112 * 512)
        this.inEvents = this.malloc(12)
        this.eventCount = 0
        this.install(this.inEvents + 4, ["i32"], ["i32"], () => this.eventCount)
        this.install(this.inEvents + 8, ["i32", "i32"], ["i32"], (_list, index) => index < this.eventCount ? this.slots + index * 112 : 0)
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
        // stereo in and out, 128 frames, and the pointer arrays the audio buffers point at
        this.audio = {
            input: [this.malloc(BLOCK * 4), this.malloc(BLOCK * 4)],
            output: [this.malloc(BLOCK * 4), this.malloc(BLOCK * 4)],
            inputChannels: this.malloc(8), outputChannels: this.malloc(8),
            inputBuffer: this.malloc(24), outputBuffer: this.malloc(24)
        }
        const {input, output, inputChannels, outputChannels, inputBuffer, outputBuffer} = this.audio
        const view = this.view
        input.forEach((pointer, index) => view.setUint32(inputChannels + index * 4, pointer, true))
        output.forEach((pointer, index) => view.setUint32(outputChannels + index * 4, pointer, true))
        for (const [buffer, channels] of [[inputBuffer, inputChannels], [outputBuffer, outputChannels]]) {
            this.bytes.fill(0, buffer, buffer + 24)
            view.setUint32(buffer, channels, true)
            view.setUint32(buffer + 8, 2, true)
        }
        this.transport = this.malloc(104)
        this.scratch = this.malloc(65536)
    }

    activate(sampleRate = SAMPLE_RATE) {
        if (!this.fn(this.plugin + 16)(this.plugin, sampleRate, 1, BLOCK)) {throw new Error("activate failed")}
        if (!this.fn(this.plugin + 24)(this.plugin)) {throw new Error("start_processing failed")}
        this.sampleRate = sampleRate
    }
    deactivate() {
        this.fn(this.plugin + 28)(this.plugin)
        this.fn(this.plugin + 20)(this.plugin)
    }
    reset() {this.fn(this.plugin + 32)(this.plugin)}
    destroy() {this.fn(this.plugin + 12)(this.plugin)}

    read(pointer) {
        const view = this.view
        const time = this.position + view.getUint32(pointer + 4, true)
        const type = view.getUint16(pointer + 10, true)
        if (type === EVENT.NOTE_ON || type === EVENT.NOTE_OFF) {
            this.out.push({time, offset: view.getUint32(pointer + 4, true), on: type === EVENT.NOTE_ON,
                channel: view.getInt16(pointer + 22, true), key: view.getInt16(pointer + 24, true),
                velocity: view.getFloat64(pointer + 32, true)})
        } else if (type === EVENT.PARAM_VALUE) {
            this.out.push({time, param: view.getUint32(pointer + 16, true), value: view.getFloat64(pointer + 40, true)})
        } else if (type === EVENT.GESTURE_BEGIN || type === EVENT.GESTURE_END) {
            this.out.push({time, gesture: type === EVENT.GESTURE_BEGIN ? "begin" : "end", param: view.getUint32(pointer + 16, true)})
        } else if (type === EVENT.MIDI) {
            this.out.push({time, midi: [...this.bytes.slice(pointer + 18, pointer + 21)]})
        }
    }

    write(events) {
        const view = this.view
        events.forEach((event, index) => {
            const pointer = this.slots + index * 112
            this.bytes.fill(0, pointer, pointer + 112)
            view.setUint32(pointer + 4, event.time, true)
            view.setUint16(pointer + 10, event.type, true)
            if (event.type === EVENT.PARAM_VALUE) {
                view.setUint32(pointer, 48, true)
                view.setUint32(pointer + 16, event.id, true)
                view.setInt32(pointer + 24, -1, true)
                view.setInt16(pointer + 28, -1, true)
                view.setInt16(pointer + 30, -1, true)
                view.setInt16(pointer + 32, -1, true)
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
        this.eventCount = events.length
    }

    writeTransport() {
        const view = this.view, pointer = this.transport
        this.bytes.fill(0, pointer, pointer + 104)
        view.setUint32(pointer, 104, true)
        view.setUint16(pointer + 10, EVENT.TRANSPORT, true)
        // has tempo, beats timeline, time signature, is playing
        view.setUint32(pointer + 16, 1 | 2 | 8 | 16, true)
        const beats = this.position / this.sampleRate * 2
        view.setBigInt64(pointer + 24, BigInt(Math.round(beats * 2 ** 31)), true)
        view.setFloat64(pointer + 40, 120, true)
        view.setUint16(pointer + 100, 4, true)
        view.setUint16(pointer + 102, 4, true)
    }

    // renders absolute sample times in blocks; sizes: the frames of each call (cycled), input: (channel, frame) => sample
    run(events, until, {sizes = [BLOCK], input = null, audioIn = true} = {}) {
        const queue = events.toSorted((first, second) => first.time - second.time)
        const left = [], right = []
        let call = 0
        while (this.position < until) {
            const frames = Math.min(sizes[call++ % sizes.length], until - this.position, BLOCK)
            const end = this.position + frames
            const block = []
            while (queue.length > 0 && queue[0].time < end) {
                const event = queue.shift()
                block.push({...event, time: Math.max(0, event.time - this.position)})
            }
            this.write(block)
            this.writeTransport()
            const view = this.view
            for (let channel = 0; channel < 2; channel++) {
                const samples = new Float32Array(this.memory.buffer, this.audio.input[channel], BLOCK)
                for (let frame = 0; frame < frames; frame++) {samples[frame] = input === null ? 0 : input(channel, this.position + frame)}
                new Float32Array(this.memory.buffer, this.audio.output[channel], BLOCK).fill(0)
            }
            view.setBigUint64(this.process, BigInt(this.position), true)
            view.setUint32(this.process + 8, frames, true)
            view.setUint32(this.process + 12, this.transport, true)
            view.setUint32(this.process + 16, audioIn ? this.audio.inputBuffer : 0, true)
            view.setUint32(this.process + 20, this.audio.outputBuffer, true)
            view.setUint32(this.process + 24, audioIn ? 1 : 0, true)
            view.setUint32(this.process + 28, 1, true)
            view.setUint32(this.process + 32, this.inEvents, true)
            view.setUint32(this.process + 36, this.outEvents, true)
            const status = this.fn(this.plugin + 36)(this.plugin, this.process)
            if (status === 0) {throw new Error(`process returned CLAP_PROCESS_ERROR at ${this.position}`)}
            this.eventCount = 0
            left.push(...new Float32Array(this.memory.buffer, this.audio.output[0], frames))
            right.push(...new Float32Array(this.memory.buffer, this.audio.output[1], frames))
            this.position = end
        }
        return [Float32Array.from(left), Float32Array.from(right)]
    }

    paramInfos() {
        const count = this.fn(this.ext.params)(this.plugin)
        const info = this.malloc(1320)
        return Array.from({length: count}, (_, index) => {
            if (!this.fn(this.ext.params + 4)(this.plugin, index, info)) {return null}
            const view = this.view
            return {
                id: view.getUint32(info, true), flags: view.getUint32(info + 4, true), name: this.cstr(info + 12),
                min: view.getFloat64(info + 1296, true), max: view.getFloat64(info + 1304, true),
                value: view.getFloat64(info + 1312, true)
            }
        })
    }
    value(id) {
        const out = this.malloc(8)
        return this.fn(this.ext.params + 8)(this.plugin, id, out) ? this.view.getFloat64(out, true) : NaN
    }
    text(id, value) {
        const out = this.malloc(256)
        return this.fn(this.ext.params + 12)(this.plugin, id, value, out, 256) ? this.cstr(out) : null
    }
    parse(id, text) {
        const out = this.malloc(8)
        return this.fn(this.ext.params + 16)(this.plugin, id, this.string(text), out) ? this.view.getFloat64(out, true) : null
    }
    flush() {
        this.write([])
        this.fn(this.ext.params + 20)(this.plugin, this.inEvents, this.outEvents)
    }
    save() {
        this.written = []
        if (!this.fn(this.ext.state)(this.plugin, this.ostream)) {throw new Error("state save failed")}
        return Uint8Array.from(this.written)
    }
    load(bytes) {
        this.source = bytes
        this.cursor = 0
        return this.fn(this.ext.state + 4)(this.plugin, this.istream)
    }
    receive(bytes) {
        this.sent = []
        this.bytes.set(bytes, this.scratch)
        const handled = this.fn(this.ext.webview + 8)(this.plugin, this.scratch, bytes.length)
        return {handled, sent: this.sent}
    }
    pageMessage(type, id = 0, value = 0) {
        const view = new DataView(new ArrayBuffer(type === PAGE.SET ? 16 : type === PAGE.POLL || type === PAGE.DESCRIBE ? 1 : 8))
        view.setUint8(0, type)
        if (view.byteLength >= 8) {view.setUint32(4, id, true)}
        if (type === PAGE.SET) {view.setFloat64(8, value, true)}
        return this.receive(new Uint8Array(view.buffer))
    }
    uri(extension) {
        const out = this.malloc(256)
        this.fn(extension)(this.plugin, out, 256)
        return this.cstr(out)
    }
}

const peakOf = channels => channels.reduce((peak, samples) => samples.reduce((max, sample) => Math.max(max, Math.abs(sample)), peak), 0)
const finite = channels => channels.every(samples => samples.every(Number.isFinite))
const tone = (channel, frame) => 0.4 * Math.sin(2 * Math.PI * 220 * frame / SAMPLE_RATE) + 0.1 * Math.sin(2 * Math.PI * 1530 * frame / SAMPLE_RATE + channel)
const randomSizes = (() => {
    let seed = 1234567
    return Array.from({length: 97}, () => {
        seed = (seed * 1103515245 + 12345) % 2147483648
        return 1 + seed % BLOCK
    })
})()

const fresh = async () => {
    const host = await Host.load(wasm.bytes)
    host.activate()
    return host
}

// --- the module ---------------------------------------------------------------------------------------------------

const module = await WebAssembly.compile(wasm.bytes)
const imports = WebAssembly.Module.imports(module)
const foreign = imports.filter(entry => entry.module !== "wasi_snapshot_preview1")
expect(foreign.length === 0, "imports WASI only",
    `imports ${foreign.map(entry => `${entry.module}.${entry.name}`).join(", ")}: only WASI is allowed (no threads, no JS)`)
const exportNames = WebAssembly.Module.exports(module).map(entry => entry.name)
expect(exportNames.includes("clap_entry") && exportNames.includes("malloc") && exportNames.includes("memory"),
    "exports clap_entry, malloc and memory", `exports: ${exportNames.join(", ")}`)

let host
try {
    host = await fresh()
} catch (error) {
    failure("loads and activates", String(error))
    report()
}
pass("loads and activates")

// --- descriptor -----------------------------------------------------------------------------------------------------

const {features, descriptor} = host
const kinds = ["instrument", "audio-effect", "note-effect"].filter(kind => features.includes(kind))
expect(kinds.length === 1, "one kind feature", `features name ${kinds.length === 0 ? "none" : kinds.join(" and ")} of instrument, audio-effect, note-effect`)
const kind = kinds[0] ?? "audio-effect"
expect(host.count === 1, "one plugin in the module", `the factory holds ${host.count}`)
expect(descriptor.id === `${CLAP_ID_PREFIX}${slug}`, "clap id", `is "${descriptor.id}", should be "${CLAP_ID_PREFIX}${slug}" (it never changes once published)`)
expect(descriptor.name === meta.name, "name matches meta.json", `descriptor "${descriptor.name}", meta.json "${meta.name}"`)
expect(descriptor.vendor.trim().length > 0, "vendor", "the vendor is empty")
expect(/^\d+\.\d+\.\d+$/.test(descriptor.version), "version", `"${descriptor.version}" is no x.y.z version`)
expect(features.includes("opendaw:device-panel"), "device panel feature", "opendaw:device-panel is missing")
const widthFeature = features.find(feature => feature.startsWith("opendaw:device-panel-width:"))
const widthOut = host.malloc(4)
const hasWidth = host.ext.panel !== 0 && host.fn(host.ext.panel + 4)(host.plugin, 176, widthOut)
const panelWidth = hasWidth ? host.u32(widthOut) : 0
expect(host.ext.panel !== 0 && hasWidth, "device panel extension", "opendaw.device-panel/1 is missing or get_width fails")
expect(widthFeature === `opendaw:device-panel-width:${panelWidth}`, "panel width feature matches get_width(176)",
    `feature ${widthFeature ?? "missing"}, get_width(176) = ${panelWidth}`)
if (panelWidth < 60 || panelWidth > 1200) {warning("panel width", `${panelWidth} px is unusual for a device panel`)}
expect(host.ext.webview !== 0 && host.uri(host.ext.webview) === "/index.html", "page (clap.webview)", "clap.webview/3 get_uri should answer /index.html")
const hasWindow = host.ext.gui !== 0
if (hasWindow) {
    const size = host.malloc(8)
    const sized = host.fn(host.ext.gui + 20)(host.plugin, size, size + 4)
    const [windowWidth, windowHeight] = [host.u32(size), host.u32(size + 4)]
    expect(sized && windowWidth >= 200 && windowHeight >= 150, `window ${windowWidth} x ${windowHeight}`,
        "the window has no sensible size: give windowWidth and windowHeight in INFO, or none for a panel-only plugin")
} else {
    pass("device panel only, no window")
}

// --- ports ------------------------------------------------------------------------------------------------------

const ports = (extension, isInput) => extension === 0 ? -1 : host.fn(extension)(host.plugin, isInput ? 1 : 0)
const expectedPorts = {
    "audio-effect": {audioIn: 1, audioOut: 1, noteIn: 0, noteOut: 0},
    "instrument": {audioIn: 0, audioOut: 1, noteIn: 1, noteOut: 0},
    "note-effect": {audioIn: 0, audioOut: 0, noteIn: 1, noteOut: 1}
}[kind]
const actualPorts = {audioIn: ports(host.ext.audioPorts, true), audioOut: ports(host.ext.audioPorts, false),
    noteIn: Math.max(0, ports(host.ext.notePorts, true)), noteOut: Math.max(0, ports(host.ext.notePorts, false))}
expect(JSON.stringify(actualPorts) === JSON.stringify(expectedPorts), `ports of a ${kind}`,
    `has ${JSON.stringify(actualPorts)}, a ${kind} has ${JSON.stringify(expectedPorts)}`)
if (kind === "audio-effect") {expect(host.ext.tail !== 0, "tail (clap.tail)", "an audio effect answers clap.tail")}
expect(host.ext.latency !== 0, "latency (clap.latency)", "clap.latency is missing")

// --- parameters ---------------------------------------------------------------------------------------------------

const params = host.paramInfos()
expect(params.length > 0 && params.every(param => param !== null), "parameters listed", "no parameters, or get_info fails")
const ids = new Set(params.map(param => param.id))
expect(ids.size === params.length && params.every(param => param.id < 2 ** 31), "parameter ids unique and below 2^31", "duplicate or too large ids")
for (const param of params) {
    const where = `parameter ${param.id} "${param.name}"`
    if (!(param.flags & FLAGS.AUTOMATABLE)) {failure(`${where} automatable`, "every parameter is automatable in openDAW")}
    if (!(param.min < param.max && param.value >= param.min && param.value <= param.max)) {
        failure(`${where} range`, `min ${param.min}, max ${param.max}, default ${param.value}`)
    }
    if (param.name.length > 12) {warning(`${where} name`, "long names make wide panels: use the shortest name that reads (Res, Smooth, Exp Ratio)")}
    const texts = [param.min, param.value, param.max].map(value => host.text(param.id, value))
    if (texts.some(text => text === null || text.length === 0)) {
        failure(`${where} texts`, "value_to_text fails for min, default or max")
        continue
    }
    if (texts.some(text => text.length > 15)) {warning(`${where} texts`, `"${texts.find(text => text.length > 15)}" is longer than 15 characters`)}
    const back = host.parse(param.id, texts[1])
    const tolerance = (param.flags & FLAGS.STEPPED) ? 0.5 : (param.max - param.min) * 0.01
    if (back === null || Math.abs(back - param.value) > tolerance) {
        failure(`${where} text round trip`, `"${texts[1]}" reads back as ${back}, not ${param.value}`)
    }
    if (host.value(param.id) !== param.value) {failure(`${where} starts at its default`, `value ${host.value(param.id)}, default ${param.value}`)}
}
pass(`${params.length} parameters with texts both ways`)

// --- state --------------------------------------------------------------------------------------------------------

{
    const first = host.save()
    const second = host.save()
    expect(first.length > 0 && first.every((byte, index) => byte === second[index]) && first.length === second.length,
        "state saves the same bytes twice", "two saves without a change differ: no counters or timestamps in the state")
    const target = params.find(param => !(param.flags & FLAGS.STEPPED)) ?? params[0]
    const next = target.flags & FLAGS.STEPPED ? (target.value + 1 <= target.max ? target.value + 1 : target.value - 1)
        : target.min + (target.max - target.min) * 0.37
    host.run([{time: host.position, type: EVENT.PARAM_VALUE, id: target.id, value: next}], host.position + BLOCK,
        {audioIn: kind === "audio-effect"})
    const edited = host.save()
    const changed = edited.length === first.length ? edited.reduce((count, byte, index) => count + (byte !== first[index] ? 1 : 0), 0) : Infinity
    expect(changed > 0 && changed <= 8, "an edit changes at most 8 state bytes",
        changed === Infinity ? "the state's length changed: keep a fixed layout" : `${changed} bytes changed`)
    expect(host.load(first) && host.value(target.id) === target.value, "state loads back", "loading the first state did not restore the value")
    const garbage = new Uint8Array(64).map((_, index) => (index * 37) & 255)
    let survived = true
    try {host.load(garbage)} catch (_) {survived = false}
    expect(survived, "a foreign state is refused safely", "loading 64 foreign bytes trapped")
    expect(!host.load(new Uint8Array(0)), "an empty state is refused", "load answered true for no bytes")
    // a damaged kit state that claims more entries than it holds
    const damaged = new Uint8Array(first.length)
    damaged.set(first)
    if (new TextDecoder().decode(first.subarray(0, 4)) === "ODKS") {new DataView(damaged.buffer).setUint32(8, 0x10000000, true)}
    let refused = false
    try {refused = !host.load(damaged)} catch (_) {}
    expect(refused, "a damaged state is refused safely", "a state with a wrong count was loaded or trapped")
    host.load(first)
}

// --- the page's messages ------------------------------------------------------------------------------------------

{
    const {sent} = host.pageMessage(PAGE.DESCRIBE)
    const description = sent.find(bytes => bytes[0] === 2)
    let described = null
    try {described = description === undefined ? null : JSON.parse(new TextDecoder().decode(description.subarray(1)))} catch (_) {}
    if (described === null && description !== undefined) {
        failure("page description", "the plugin's description is no valid JSON")
    } else if (described === null) {
        warning("page description", "no answer to the kit's describe message: fine for a page of your own, the kit page needs it")
    } else {
        expect(described.params.length === params.length && described.width === panelWidth, "page description",
            `${described.params.length} parameters and width ${described.width}, the plugin has ${params.length} and ${panelWidth}`)
    }
    const poll = host.pageMessage(PAGE.POLL)
    expect(poll.handled && poll.sent.length === 1 && poll.sent[0][0] === 1, "poll answered with one status message",
        `answered ${poll.sent.length} messages`)
    if (poll.sent.length === 1 && poll.sent[0].length > 4096) {warning("status size", `${poll.sent[0].length} bytes per poll: send only what changed`)}
    const target = params[params.length - 1]
    const value = target.flags & FLAGS.STEPPED ? target.min : target.max
    host.hovered = []
    host.pageMessage(PAGE.HOVER, target.id)
    expect(host.hovered.at(-1) === target.id, "hover reaches the host", "clap.param-hovered was not updated")
    const dirtyBefore = host.dirty
    host.out = []
    host.pageMessage(PAGE.BEGIN, target.id)
    host.pageMessage(PAGE.SET, target.id, value)
    const dirtyDuring = host.dirty
    host.pageMessage(PAGE.END, target.id)
    host.run([], host.position + BLOCK, {audioIn: kind === "audio-effect"})
    const gestures = host.out.filter(event => event.gesture !== undefined || event.param !== undefined)
    expect(gestures[0]?.gesture === "begin" && gestures.at(-1)?.gesture === "end" &&
        gestures.some(event => event.gesture === undefined && event.value === value),
    "a drag reaches the host as one gesture", `events out: ${JSON.stringify(gestures)}`)
    expect(dirtyDuring === dirtyBefore && host.dirty === dirtyBefore + 1, "state marked dirty once, at the drag's end",
        `marked ${dirtyDuring - dirtyBefore} times during the drag, ${host.dirty - dirtyDuring} at its end`)
    expect(host.value(target.id) === value, "the page's value is the plugin's", `value ${host.value(target.id)}, page sent ${value}`)
}

// --- a change from the page sounds as the same change from the host ------------------------------------------------

{
    const target = params.find(param => !(param.flags & FLAGS.STEPPED)) ?? params[0]
    const value = target.flags & FLAGS.STEPPED ? (target.value === target.min ? target.max : target.min)
        : target.min + (target.max - target.min) * 0.8
    const notesIn = kind === "audio-effect" ? [] : [{time: 64, type: EVENT.NOTE_ON, key: 60}, {time: 9000, type: EVENT.NOTE_OFF, key: 60}]
    const options = {input: kind === "audio-effect" ? tone : null, audioIn: kind === "audio-effect"}
    const viaHost = await fresh()
    const hostOut = viaHost.run([{time: 0, type: EVENT.PARAM_VALUE, id: target.id, value}, ...notesIn], SAMPLE_RATE / 2, options)
    const viaPage = await fresh()
    viaPage.pageMessage(PAGE.BEGIN, target.id)
    viaPage.pageMessage(PAGE.SET, target.id, value)
    viaPage.pageMessage(PAGE.END, target.id)
    viaPage.out = []
    const pageOut = viaPage.run(notesIn, SAMPLE_RATE / 2, options)
    let difference = 0
    for (let channel = 0; channel < 2; channel++) {
        for (let frame = 0; frame < hostOut[channel].length; frame++) {
            difference = Math.max(difference, Math.abs(hostOut[channel][frame] - pageOut[channel][frame]))
        }
    }
    const notesOf = local => JSON.stringify(local.out.filter(event => event.key !== undefined).map(({time, on, key}) => [time, on, key]))
    expect(difference < 1e-6 && notesOf(viaHost) === notesOf(viaPage), `a change on the page reaches the sound ("${target.name}")`,
        `the page's change and the host's change of the same value sound different (up to ${difference.toExponential(2)}): apply page values before rendering`)
}

// --- rendering ----------------------------------------------------------------------------------------------------

const chord = start => [
    {time: start, type: EVENT.NOTE_ON, key: 48, velocity: 0.8}, {time: start + 9000, type: EVENT.NOTE_ON, key: 55, velocity: 0.6},
    {time: start + 17011, type: EVENT.NOTE_ON, key: 64, velocity: 0.9},
    {time: start + 30000, type: EVENT.NOTE_OFF, key: 48}, {time: start + 30001, type: EVENT.NOTE_OFF, key: 55},
    {time: start + 30517, type: EVENT.NOTE_OFF, key: 64}
]
const automation = (start, length) => params.filter(param => !(param.flags & FLAGS.STEPPED)).slice(0, 3).flatMap((param, index) =>
    Array.from({length: 6}, (_, step) => ({
        time: start + Math.round((step + 1) * length / 7) + index * 13, type: EVENT.PARAM_VALUE, id: param.id,
        value: param.min + (param.max - param.min) * ((step * 0.37 + index * 0.21) % 1)
    })))

if (kind !== "note-effect") {
    const render = async sizes => {
        const local = await fresh()
        const length = SAMPLE_RATE * 2
        const events = [...(kind === "instrument" ? chord(1000) : []), ...automation(0, length)]
        const memoryBefore = local.memory.buffer.byteLength
        const channels = local.run(events, length, {sizes, input: kind === "audio-effect" ? tone : null, audioIn: kind === "audio-effect"})
        return {local, channels, grew: local.memory.buffer.byteLength > memoryBefore}
    }
    const whole = await render([BLOCK])
    const split = await render(randomSizes)
    expect(finite(whole.channels) && finite(split.channels), "output is finite", "NaN or infinity in the output")
    const peak = peakOf(whole.channels)
    if (kind === "instrument") {expect(peak > 0.001, "notes make sound", `the output peaks at ${peak} while notes play`)}
    else if (peak < 0.001) {warning("output", "the effect is silent with its default settings")}
    if (peak > 1.5) {warning("output level", `peaks at ${peak.toFixed(2)} (${(20 * Math.log10(peak)).toFixed(1)} dB) with default settings`)}
    let difference = 0
    for (let channel = 0; channel < 2; channel++) {
        for (let frame = 0; frame < whole.channels[channel].length; frame++) {
            difference = Math.max(difference, Math.abs(whole.channels[channel][frame] - split.channels[channel][frame]))
        }
    }
    expect(difference < 1e-4, "the same at any block size (events at their sample)",
        `128-frame blocks and random blocks from 1 to 128 frames differ by up to ${difference.toExponential(2)}: count time in samples, never in calls, and smooth per sample`)
    expect(!whole.grew && !split.grew, "no allocation while processing", "memory grew during process: allocate in activate only")
    // reset silences everything
    const after = whole.local
    after.reset()
    const silence = after.run([], after.position + SAMPLE_RATE / 2, {audioIn: kind === "audio-effect"})
    const tail = silence.map(samples => samples.subarray(Math.round(SAMPLE_RATE * 0.01)))
    expect(peakOf(tail) < 1e-4, "silent after reset", `still ${peakOf(tail).toExponential(2)} after reset with no input: reset clears voices, delays and filters`)
    if (kind === "instrument") {
        const idle = await fresh()
        const quiet = idle.run([], SAMPLE_RATE / 2, {audioIn: false})
        expect(peakOf(quiet) === 0, "no sound without notes", "the instrument sounds before any note")
        const done = await fresh()
        done.run(chord(0), SAMPLE_RATE, {audioIn: false})
        const late = done.run([], done.position + SAMPLE_RATE * 6, {audioIn: false}).map(samples => samples.subarray(SAMPLE_RATE * 5))
        expect(peakOf(late) < 1e-4, "voices end after their release", "still sounding 5 s after the last note off")
    }
    // other sample rates
    for (const rate of [44100, 96000]) {
        const other = await Host.load(wasm.bytes)
        other.activate(rate)
        const channels = other.run(kind === "instrument" ? chord(100) : [], rate / 2, {input: kind === "audio-effect" ? tone : null, audioIn: kind === "audio-effect"})
        expect(finite(channels), `runs at ${rate} Hz`, "NaN or infinity at this sample rate")
    }
    // speed: microseconds per 128-frame block
    const timed = await fresh()
    const notes = kind === "instrument" ? [36, 43, 48, 52, 55, 60, 64, 67].map((key, index) => ({time: index, type: EVENT.NOTE_ON, key})) : []
    timed.run(notes, BLOCK * 8, {input: kind === "audio-effect" ? tone : null, audioIn: kind === "audio-effect"})
    const times = []
    for (let index = 0; index < 400; index++) {
        const start = performance.now()
        timed.run([], timed.position + BLOCK, {audioIn: kind === "audio-effect"})
        times.push(performance.now() - start)
    }
    times.sort((first, second) => first - second)
    const median = times[times.length >> 1] * 1000
    // the host's own buffer filling is part of the figure, so it is an upper bound
    if (median > 1000) {failure("speed", `${median.toFixed(0)} µs per 128 frames (a block lasts 2667 µs, openDAW runs every device in it)`)}
    else if (median > 250) {warning("speed", `${median.toFixed(0)} µs per 128 frames: heavy, measure where the time goes`)}
    else {pass(`speed: ${median.toFixed(0)} µs per 128 frames (upper bound)`)}
} else {
    const local = await fresh()
    const notesIn = [...chord(100), {time: 40000, type: EVENT.NOTE_ON, key: 60}, {time: 40000, type: EVENT.NOTE_ON, key: 64},
        {time: 41000, type: EVENT.NOTE_ON, key: 60}, {time: 50000, type: EVENT.NOTE_OFF, key: 60},
        {time: 50001, type: EVENT.NOTE_OFF, key: 60}, {time: 51000, type: EVENT.NOTE_OFF, key: 64},
        {time: 42000, type: EVENT.MIDI, data: [0xb0, 64, 127]}]
    local.run([...notesIn, ...automation(0, 60000)], 60000, {sizes: randomSizes, audioIn: false})
    const notes = local.out.filter(event => event.key !== undefined)
    let sorted = true, last = -1
    const sounding = new Map()
    let wellFormed = true, problem = ""
    for (const note of notes) {
        if (note.time < last) {sorted = false}
        last = note.time
        const key = `${note.channel}:${note.key}`
        const count = sounding.get(key) ?? 0
        if (note.on && count > 0) {wellFormed = false; problem ||= `key ${key} struck while sounding at ${note.time}`}
        if (!note.on && count === 0) {wellFormed = false; problem ||= `note off without note on for ${key} at ${note.time}`}
        sounding.set(key, note.on ? 1 : 0)
    }
    expect(notes.length > 0, "notes go out", "no notes came out for notes in")
    expect(sorted, "notes out sorted by time", "the output events are not sorted by time")
    expect(wellFormed, "a key never held twice, every off has its on", problem)
    const held = [...sounding.entries()].filter(([, count]) => count > 0).map(([key]) => key)
    expect(held.length === 0, "every note on is ended", `still sounding after all notes ended: ${held.join(", ")}`)
    expect(local.out.some(event => event.midi?.[0] === 0xb0), "other MIDI passes through", "the sustain pedal (CC 64) did not come out")
    local.reset()
    pass("reset")
    // the same notes at any block size, with the transport running (tempo-synced patterns count frames, not calls)
    const notesOut = async sizes => {
        const other = await fresh()
        other.run([...notesIn, ...automation(0, 60000)], 60000, {sizes, audioIn: false})
        return other.out.filter(event => event.key !== undefined)
    }
    // a step on the song's grid may land a sample apart, the transport's position is rounded per call
    const whole = await notesOut([BLOCK]), split = await notesOut(randomSizes)
    const same = whole.length === split.length && whole.every((note, index) => note.on === split[index].on &&
        note.key === split[index].key && note.channel === split[index].channel && Math.abs(note.time - split[index].time) <= 1)
    const first = whole.findIndex((note, index) => split[index] === undefined || note.on !== split[index].on ||
        note.key !== split[index].key || Math.abs(note.time - split[index].time) > 1)
    const show = note => note === undefined ? "nothing" : `${note.on ? "on" : "off"} ${note.key} at ${note.time}`
    expect(same, "the same notes at any block size (events at their sample)",
        `128-frame blocks and random blocks from 1 to 128 frames give other notes (${whole.length} and ${split.length}, first: ${show(whole[first])} against ${show(split[first])}): count time in frames, never in calls`)
}

// --- latency ------------------------------------------------------------------------------------------------------

{
    const latency = host.fn(host.ext.latency)(host.plugin)
    if (latency > 0) {warning("latency", `${latency} frames of latency: openDAW compensates it, keep it as small as the sound allows`)}
}

// --- the page's files ---------------------------------------------------------------------------------------------

{
    const page = fileOf("index.html")
    expect(page !== undefined, "page index.html", "the bundle has no index.html")
    if (page !== undefined) {
        const html = new TextDecoder().decode(page.bytes)
        const references = [...html.matchAll(/(?:src|href)\s*=\s*"([^"]+)"/g)].map(match => match[1])
        for (const reference of references) {
            if (/^https?:/.test(reference)) {
                if (!/^https:\/\/fonts\.(googleapis|gstatic)\.com(\/|$)/.test(reference)) {
                    failure("page loads only its own files", `${reference}: no network of its own (Google Fonts is fine)`)
                }
                continue
            }
            const local = reference.replace(/^\.\//, "").split(/[?#]/)[0]
            if (local.length > 0 && !local.startsWith("data:") && fileOf(local) === undefined) {
                failure("page files present", `index.html loads ${reference}, the bundle has no such file`)
            }
        }
        // the page's own code, not the kit's
        const own = [html, ...files.filter(file => file.path.endsWith(".js") && !/(^|\/)(panel|opendaw-kit)\.js$/.test(file.path))
            .map(file => new TextDecoder().decode(file.bytes))].join("\n")
        const lints = [
            [/addEventListener\(\s*["']wheel["']|onwheel/, "a wheel handler: the wheel scrolls openDAW's devices, values change by dragging"],
            [/addEventListener\(\s*["']dblclick["']|ondblclick/, "a double-click handler: double-click is openDAW's value entry"],
            [/addEventListener\(\s*["']contextmenu["']|oncontextmenu/, "a context menu handler: right-click is openDAW's parameter menu"],
            [/requestPointerLock/, "pointer lock: it stops openDAW's double-click and menus"],
            [/localStorage|sessionStorage|indexedDB/, "browser storage: everything that matters belongs in parameters (the state)"],
            [/\bfetch\(|XMLHttpRequest|WebSocket/, "network calls: a page loads only its own files"],
            [/key(?:down|up|press)/, "key handlers: plain keys play notes in openDAW, never build on keys"]
        ]
        for (const [pattern, reason] of lints) {
            if (pattern.test(own)) {warning("page", reason)}
        }
        if (hasWindow && fileOf("opendaw-kit.js") !== undefined && !/window\s*:\s*\{/.test(html)) {
            failure("window content", "the plugin has a window but index.html gives OpenDAWKit.start no window: build content the panel cannot show, or remove windowWidth and windowHeight from INFO")
        }
        if (!hasWindow && /window\s*:\s*\{[^}]*build/.test(html)) {
            warning("window content", "index.html builds a window, but the plugin has none (windowWidth and windowHeight in INFO)")
        }
        if (!/opendaw-kit\.js|param-hovered|PAGE\.HOVER|\.hover\(/.test(own) && fileOf("opendaw-kit.js") === undefined) {
            warning("page", "no hover reports found: without them openDAW's right-click menu and value entry do not work on the page")
        }
    }
}

if (!Array.isArray(meta.tags) || meta.tags.length === 0) {warning("meta.json tags", "no tags: add a few lowercase words joined by hyphens (for example lo-fi, chorus, bass) so people find it")}
if (host.memoryRef.log.length > 0) {warning("plugin log", host.memoryRef.log.slice(0, 5).join(" | "))}
report()

function report() {
    for (const result of results) {
        if (result.kind === "pass") {console.log(`✓ ${result.name}`)}
        else if (result.kind === "warn") {console.log(`! ${result.name}: ${result.reason}`)}
        else {console.log(`✗ ${result.name}: ${result.reason}`)}
    }
    const failures = results.filter(result => result.kind === "fail").length
    const warnings = results.filter(result => result.kind === "warn").length
    console.log(`\n${failures === 0 ? "✓" : "✗"} ${slug}: ${results.filter(result => result.kind === "pass").length} passed, ${failures} failed, ${warnings} warnings`)
    process.exit(failures === 0 ? 0 : 1)
}
