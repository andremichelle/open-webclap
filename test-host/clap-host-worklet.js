// open-webclap test host, audio side: runs a WebCLAP module.wasm in an AudioWorklet.
//
// A minimal single-threaded CLAP host, enough to test a port: notes, parameters (in and out), state, audio
// ports (inputs from the node's inputs), clap.gui with the webview API and clap.webview. Calls that CLAP declares main-thread (receive, state,
// gui) run here too, between process calls, like in openDAW. Struct offsets are wasm32 (checked against
// the CLAP headers with emcc).

"use strict"

const QUANTUM = 128
const EVENT_SLOT = 48         // bytes per input event slot (the largest event we send, clap_event_param_value)
const MAX_EVENTS = 512
const SCRATCH_BYTES = 1 << 20 // for page messages and state blobs

const EVENT = {NOTE_ON: 0, NOTE_OFF: 1, NOTE_CHOKE: 2, PARAM_VALUE: 5, GESTURE_BEGIN: 7, GESTURE_END: 8, MIDI: 10}
const EXT = {
    PARAMS: "clap.params", STATE: "clap.state", GUI: "clap.gui", WEBVIEW: "clap.webview/3",
    AUDIO_PORTS: "clap.audio-ports", NOTE_PORTS: "clap.note-ports", LOG: "clap.log", THREAD_CHECK: "clap.thread-check",
    DEVICE_PANEL: "opendaw.device-panel/1"
}
const PANEL_HEIGHT = 176 // openDAW's device panel surface

// The notes a note effect sends out, played sample exact: a sine per key, 5 ms attack, 120 ms release
class SineVoices {
    constructor() { this.voices = new Map() }
    start(key, velocity) {
        this.voices.set(key, {phase: 0, step: 440 * 2 ** ((key - 69) / 12) / sampleRate, level: 0,
            target: 0.2 * velocity, rate: 1 / (0.005 * sampleRate)})
    }
    stop(key) {
        const voice = this.voices.get(key)
        if (voice) { voice.target = 0; voice.rate = voice.level / (0.12 * sampleRate) }
    }
    clear() { this.voices.clear() }
    render(left, right, from, to) {
        for (const [key, voice] of this.voices) {
            for (let i = from; i < to; i++) {
                voice.level = voice.level < voice.target ? Math.min(voice.target, voice.level + voice.rate * voice.target)
                    : Math.max(voice.target, voice.level - voice.rate)
                const sample = Math.sin(voice.phase * 2 * Math.PI) * voice.level
                voice.phase = (voice.phase + voice.step) % 1
                left[i] += sample
                right[i] += sample
            }
            if (voice.target === 0 && voice.level === 0) this.voices.delete(key)
        }
    }
}

// AudioWorkletGlobalScope has no TextEncoder/TextDecoder
function encodeUtf8(text) {
    const bytes = []
    for (const ch of text) {
        const c = ch.codePointAt(0)
        if (c < 0x80) bytes.push(c)
        else if (c < 0x800) bytes.push(0xc0 | c >> 6, 0x80 | c & 63)
        else if (c < 0x10000) bytes.push(0xe0 | c >> 12, 0x80 | c >> 6 & 63, 0x80 | c & 63)
        else bytes.push(0xf0 | c >> 18, 0x80 | c >> 12 & 63, 0x80 | c >> 6 & 63, 0x80 | c & 63)
    }
    return new Uint8Array(bytes)
}

function decodeUtf8(bytes) {
    let text = ""
    for (let i = 0; i < bytes.length;) {
        const b = bytes[i++]
        let c = b
        if (b >= 0xf0) c = (b & 7) << 18 | (bytes[i++] & 63) << 12 | (bytes[i++] & 63) << 6 | bytes[i++] & 63
        else if (b >= 0xe0) c = (b & 15) << 12 | (bytes[i++] & 63) << 6 | bytes[i++] & 63
        else if (b >= 0xc0) c = (b & 31) << 6 | bytes[i++] & 63
        text += String.fromCodePoint(c)
    }
    return text
}

// A wasm function wrapping a JS function, so it can go into the plugin's function table.
const VALTYPE = {i32: 0x7f, i64: 0x7e, f32: 0x7d, f64: 0x7c}
function trampoline(params, results, fn) {
    const name = s => [s.length, ...[...s].map(c => c.charCodeAt(0))]
    const section = (id, bytes) => [id, bytes.length, ...bytes]
    const type = [1, 0x60, params.length, ...params.map(t => VALTYPE[t]), results.length, ...results.map(t => VALTYPE[t])]
    const bytes = new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        ...section(1, type),
        ...section(2, [1, ...name("e"), ...name("f"), 0x00, 0x00]),
        ...section(7, [1, ...name("f"), 0x00, 0x00])])
    return new WebAssembly.Instance(new WebAssembly.Module(bytes), {e: {f: fn}}).exports.f
}

// WASI preview1: console output, clocks, random. No files: no preopened directories (Rust's and wasi-libc's
// startup asks for them until EBADF), every other file call fails.
function wasiImports(module, memory, log) {
    const view = () => new DataView(memory().buffer)
    const lines = ["", "", ""]
    const shim = {
        args_get: () => 0, environ_get: () => 0,
        args_sizes_get: (count, size) => (view().setUint32(count, 0, true), view().setUint32(size, 0, true), 0),
        environ_sizes_get: (count, size) => (view().setUint32(count, 0, true), view().setUint32(size, 0, true), 0),
        // currentTime stands still during a call, so both clocks use the wall clock (worklets have no performance.now)
        clock_time_get: (_id, _precision, out) => (view().setBigUint64(out, BigInt(Date.now()) * 1000000n, true), 0),
        fd_write: (fd, iovs, count, written) => {
            const v = view(), bytes = new Uint8Array(memory().buffer)
            let text = "", total = 0
            for (let i = 0; i < count; i++) {
                const ptr = v.getUint32(iovs + i * 8, true), len = v.getUint32(iovs + i * 8 + 4, true)
                text += decodeUtf8(bytes.subarray(ptr, ptr + len))
                total += len
            }
            v.setUint32(written, total, true)
            if (fd === 1 || fd === 2) {
                const parts = (lines[fd] + text).split("\n")
                lines[fd] = parts.pop()
                parts.forEach(line => log(line))
            }
            return 0
        },
        fd_prestat_get: () => 8, // EBADF
        fd_close: () => 8,
        fd_fdstat_get: (fd, out) => {
            if (fd > 2) return 8
            new Uint8Array(memory().buffer).fill(0, out, out + 24)
            view().setUint8(out, 2)
            return 0
        },
        proc_exit: code => { throw new Error(`plugin exited with ${code}`) },
        // Worklets have no crypto: seeds for oscillator drift and randomize buttons, nothing secret
        random_get: (ptr, len) => {
            const bytes = new Uint8Array(memory().buffer, ptr, len)
            for (let i = 0; i < len; i++) bytes[i] = Math.random() * 256
            return 0
        },
        sched_yield: () => 0
    }
    const imports = {}
    for (const entry of WebAssembly.Module.imports(module)) {
        if (entry.kind !== "function") continue
        const ns = imports[entry.module] || (imports[entry.module] = {})
        ns[entry.name] = shim[entry.name] || (() => {
            log(`unsupported import ${entry.module}.${entry.name}`)
            return 52 // ENOSYS
        })
    }
    return imports
}

class ClapHost extends AudioWorkletProcessor {
    constructor() {
        super()
        this.ready = false
        this.events = []          // [{type, ...}] for the next process call
        this.mainThreadRequested = false
        this.flushRequested = false
        this.honourCallbacks = true
        this.guiOpen = false
        this.hasPanel = false
        this.noteEffect = false
        this.notesOut = []        // the note effect's output of the current call, [{offset, on, channel, key, velocity} | {offset, midi}]
        this.sine = new SineVoices()
        this.playSine = true
        this.port.onmessage = event => this.handle(event.data).catch(error => this.fail(error))
    }

    fail(error) {
        this.ready = false
        this.port.postMessage({type: "error", message: String(error && error.stack || error)})
    }

    log(line) { this.port.postMessage({type: "log", line}) }

    // ---------------------------------------------------------------------------------------------------------
    // Memory helpers

    get view() { return new DataView(this.memory.buffer) }
    get u8() { return new Uint8Array(this.memory.buffer) }
    u32(ptr) { return this.view.getUint32(ptr, true) }
    fn(ptr) { return this.table.get(this.u32(ptr)) }
    cstr(ptr) {
        const bytes = this.u8
        let end = ptr
        while (bytes[end] !== 0) end++
        return decodeUtf8(bytes.subarray(ptr, end))
    }
    alloc(size) { return this.malloc(size) }
    allocString(text) {
        const bytes = encodeUtf8(text)
        const ptr = this.alloc(bytes.length + 1)
        this.u8.set(bytes, ptr)
        this.u8[ptr + bytes.length] = 0
        return ptr
    }
    install(ptr, params, results, fn) {
        const index = this.table.grow(1)
        this.table.set(index, trampoline(params, results, fn))
        this.view.setUint32(ptr, index, true)
    }
    copyIn(bytes) {
        if (bytes.byteLength > SCRATCH_BYTES) throw new Error(`message too large: ${bytes.byteLength}`)
        this.u8.set(new Uint8Array(bytes), this.scratch)
        return this.scratch
    }

    // ---------------------------------------------------------------------------------------------------------
    // Loading

    async handle(m) {
        switch (m.type) {
            case "load": return this.load(m)
            case "openGui": return this.openGui()
            case "closeGui": return this.closeGui()
            case "receive":
                if (!this.ready || !this.webview) return
                this.fn(this.webview + 8)(this.plugin, this.copyIn(m.bytes), m.bytes.byteLength)
                break
            case "param":
                this.events.push({type: EVENT.PARAM_VALUE, id: m.id, value: m.value})
                break
            case "note":
                this.events.push({type: m.on ? EVENT.NOTE_ON : EVENT.NOTE_OFF, key: m.key, velocity: m.velocity})
                break
            case "midi":
                this.events.push({type: EVENT.MIDI, data: m.data})
                break
            case "saveState": return this.port.postMessage({type: "state", bytes: this.saveState()})
            case "loadState": this.loadState(m.bytes); break
            case "valueText": return this.port.postMessage({type: "valueText", id: m.id, text: this.valueText(m.id, m.value)})
            case "honourCallbacks": this.honourCallbacks = m.value; break
            case "playSine":
                this.playSine = m.value
                if (!m.value) this.sine.clear()
                break
        }
        this.afterMainThreadCall()
    }

    async load({bytes, pluginIndex = 0}) {
        const module = await WebAssembly.compile(bytes)
        let memory = null
        const instance = await WebAssembly.instantiate(module, wasiImports(module, () => memory, line => this.log(line)))
        const exports = instance.exports
        memory = exports.memory
        if (!memory || !exports.__indirect_function_table || !exports.malloc || !exports.clap_entry)
            throw new Error("module lacks memory, __indirect_function_table, malloc or clap_entry")
        if (exports._initialize) exports._initialize()
        this.memory = memory
        this.table = exports.__indirect_function_table
        this.malloc = exports.malloc

        const entry = exports.clap_entry.value
        if (!this.fn(entry + 12)(this.allocString("/"))) throw new Error("clap_entry.init failed")
        const factory = this.fn(entry + 20)(this.allocString("clap.plugin-factory"))
        if (!factory) throw new Error("no plugin factory")
        const descriptor = this.fn(factory + 4)(factory, pluginIndex)
        const id = this.cstr(this.u32(descriptor + 12))

        this.createHost()
        this.plugin = this.fn(factory + 8)(factory, this.host, this.allocString(id))
        if (!this.plugin) throw new Error(`create_plugin(${id}) failed`)
        if (!this.fn(this.plugin + 8)(this.plugin)) throw new Error("plugin.init failed")

        const extension = name => this.fn(this.plugin + 40)(this.plugin, this.allocString(name))
        this.params = extension(EXT.PARAMS)
        this.state = extension(EXT.STATE)
        this.gui = extension(EXT.GUI)
        this.webview = extension(EXT.WEBVIEW)
        this.audioPorts = extension(EXT.AUDIO_PORTS)
        this.notePorts = extension(EXT.NOTE_PORTS)
        const devicePanel = extension(EXT.DEVICE_PANEL)

        this.createProcess()
        if (!this.fn(this.plugin + 16)(this.plugin, sampleRate, 1, QUANTUM)) throw new Error("plugin.activate failed")
        this.fn(this.plugin + 24)(this.plugin)
        this.ready = true

        const features = []
        for (let p = this.u32(descriptor + 44); p && this.u32(p); p += 4) features.push(this.cstr(this.u32(p)))
        const noteOutputs = this.notePorts ? this.fn(this.notePorts)(this.plugin, 0) : 0
        const panel = this.panelInfo(devicePanel, features)
        this.hasPanel = panel !== null
        this.noteEffect = features.includes("note-effect") || (noteOutputs > 0 && this.outputPorts.length === 0)
        this.port.postMessage({
            type: "loaded",
            descriptor: {id, name: this.cstr(this.u32(descriptor + 16)), vendor: this.cstr(this.u32(descriptor + 20)),
                version: this.cstr(this.u32(descriptor + 36)), features},
            params: this.paramInfos(), hasGui: !!(this.gui && this.webview), memoryBytes: memory.buffer.byteLength,
            audioInputs: this.inputPorts.map(port => port.info), audioOutputs: this.outputPorts.map(port => port.info),
            noteOutputs, noteEffect: this.noteEffect, panel
        })
    }

    // the device panel page and its width at openDAW's panel height, null without feature or extension
    panelInfo(devicePanel, features) {
        if (!devicePanel || !features.includes("opendaw:device-panel")) return null
        const length = this.fn(devicePanel)(this.plugin, this.textPtr, 256)
        const width = this.alloc(4)
        if (length <= 0 || !this.fn(devicePanel + 4)(this.plugin, PANEL_HEIGHT, width)) return null
        return {uri: this.cstr(this.textPtr), width: this.u32(width), height: PANEL_HEIGHT}
    }

    createHost() {
        const host = this.alloc(48)
        const extensions = new Map()
        const ext = (name, size) => { const ptr = this.alloc(size); extensions.set(name, ptr); return ptr }
        const strings = ["open-webclap test host", "open-webclap", "", "0.1"].map(text => this.allocString(text))
        const v = this.view // after the allocations, see createProcess
        v.setUint32(host, 1, true); v.setUint32(host + 4, 2, true); v.setUint32(host + 8, 0, true)
        v.setUint32(host + 12, 0, true)
        strings.forEach((ptr, i) => v.setUint32(host + 16 + i * 4, ptr, true))
        this.install(host + 32, ["i32", "i32"], ["i32"], (_h, idPtr) => extensions.get(this.cstr(idPtr)) || 0)
        this.install(host + 36, ["i32"], [], () => this.log("request_restart (ignored)"))
        this.install(host + 40, ["i32"], [], () => {})
        this.install(host + 44, ["i32"], [], () => { this.mainThreadRequested = true })

        const params = ext("clap.params", 12)
        this.install(params, ["i32", "i32"], [], () => this.port.postMessage({type: "params", params: this.paramInfos()}))
        this.install(params + 4, ["i32", "i32", "i32"], [], () => {})
        this.install(params + 8, ["i32"], [], () => { this.flushRequested = true })

        const state = ext("clap.state", 4)
        this.install(state, ["i32"], [], () => this.port.postMessage({type: "dirty"}))

        const gui = ext("clap.gui", 20)
        this.install(gui, ["i32"], [], () => {})
        this.install(gui + 4, ["i32", "i32", "i32"], ["i32"], (_h, width, height) => {
            this.port.postMessage({type: "resize", width, height})
            return 1
        })
        // request_show: a device panel page asks for the window, a host without panels has nothing to show
        this.install(gui + 8, ["i32"], ["i32"], () => {
            if (!this.hasPanel) return 0
            this.port.postMessage({type: "requestShow"})
            return 1
        })
        this.install(gui + 12, ["i32"], ["i32"], () => 0)
        this.install(gui + 16, ["i32", "i32"], [], () => {})

        const webview = ext("clap.webview/3", 4)
        this.install(webview, ["i32", "i32", "i32"], ["i32"], (_h, ptr, size) => {
            if (!this.guiOpen) return 0
            const bytes = this.u8.slice(ptr, ptr + size).buffer
            this.port.postMessage({type: "send", bytes}, [bytes])
            return 1
        })

        const log = ext("clap.log", 4)
        this.install(log, ["i32", "i32", "i32"], [], (_h, severity, msg) => this.log(`(${severity}) ${this.cstr(msg)}`))

        const threadCheck = ext("clap.thread-check", 8)
        this.install(threadCheck, ["i32"], ["i32"], () => this.inProcess ? 0 : 1)
        this.install(threadCheck + 4, ["i32"], ["i32"], () => this.inProcess ? 1 : 0)
        this.host = host
    }

    // A DataView is taken after the allocations it writes into: malloc can grow the memory, which detaches the
    // buffer of every view taken before.
    // The plugin's audio ports, without clap.audio-ports a stereo output. Effects take their inputs from the
    // node's inputs, port by port (main first, then sidechains); unconnected inputs are silent.
    portInfos(isInput) {
        if (!this.audioPorts) return isInput ? [] : [{name: "Output", channels: 2, main: true}]
        const count = this.fn(this.audioPorts)(this.plugin, isInput ? 1 : 0)
        const info = this.alloc(276)
        const list = []
        for (let i = 0; i < count; i++) {
            if (!this.fn(this.audioPorts + 4)(this.plugin, i, isInput ? 1 : 0, info)) continue
            list.push({name: this.cstr(info + 4), channels: this.u32(info + 264), main: (this.u32(info + 260) & 1) !== 0})
        }
        return list
    }

    // clap_audio_buffer[] for the ports, 24 bytes each, the channels QUANTUM floats each
    createPorts(infos) {
        const buffers = this.alloc(Math.max(1, infos.length) * 24)
        const ports = infos.map(info => {
            const data = this.alloc(info.channels * 4)
            const channels = Array.from({length: info.channels}, () => this.alloc(QUANTUM * 4))
            return {info, data, channels}
        })
        const v = this.view
        ports.forEach(({data, channels}, i) => {
            const buffer = buffers + i * 24
            channels.forEach((channel, ch) => v.setUint32(data + ch * 4, channel, true))
            v.setUint32(buffer, data, true)
            v.setUint32(buffer + 4, 0, true)
            v.setUint32(buffer + 8, channels.length, true)
            v.setUint32(buffer + 12, 0, true)
            v.setBigUint64(buffer + 16, 0n, true)
        })
        return {buffers, ports}
    }

    createProcess() {
        this.processPtr = this.alloc(40)
        const inputs = this.createPorts(this.portInfos(true))
        const outputs = this.createPorts(this.portInfos(false))
        this.inputPorts = inputs.ports
        this.outputPorts = outputs.ports
        const audioIn = inputs.buffers, audioOut = outputs.buffers
        let v = this.view
        this.eventSlots = this.alloc(EVENT_SLOT * MAX_EVENTS)
        this.eventCount = 0
        this.inEvents = this.alloc(12)
        this.install(this.inEvents + 4, ["i32"], ["i32"], () => this.eventCount)
        this.install(this.inEvents + 8, ["i32", "i32"], ["i32"], (_l, index) =>
            index < this.eventCount ? this.eventSlots + index * EVENT_SLOT : 0)
        this.outEvents = this.alloc(8)
        this.install(this.outEvents + 4, ["i32", "i32"], ["i32"], (_l, ptr) => (this.outputEvent(ptr), 1))

        v = this.view
        v.setBigUint64(this.processPtr, 0n, true)
        v.setUint32(this.processPtr + 12, 0, true)          // no transport
        v.setUint32(this.processPtr + 16, audioIn, true)
        v.setUint32(this.processPtr + 20, audioOut, true)
        v.setUint32(this.processPtr + 32, this.inEvents, true)
        v.setUint32(this.processPtr + 36, this.outEvents, true)
        this.scratch = this.alloc(SCRATCH_BYTES)
        this.textPtr = this.alloc(256)
        this.valuePtr = this.alloc(8)
        this.infoPtr = this.alloc(1320)
        this.steadyTime = 0n

        this.ostream = this.alloc(8)
        this.install(this.ostream + 4, ["i32", "i32", "i64"], ["i64"], (_s, ptr, size) => {
            this.writeChunks.push(this.u8.slice(ptr, ptr + Number(size)))
            return size
        })
        this.istream = this.alloc(8)
        this.install(this.istream + 4, ["i32", "i32", "i64"], ["i64"], (_s, ptr, size) => {
            const source = this.readSource
            const n = Math.min(Number(size), source.bytes.length - source.cursor)
            this.u8.set(source.bytes.subarray(source.cursor, source.cursor + n), ptr)
            source.cursor += n
            return BigInt(n)
        })
    }

    // ---------------------------------------------------------------------------------------------------------
    // Parameters, state, gui

    paramInfos() {
        if (!this.params) return []
        const count = this.fn(this.params)(this.plugin)
        const list = []
        for (let i = 0; i < count; i++) {
            if (!this.fn(this.params + 4)(this.plugin, i, this.infoPtr)) continue
            const v = this.view, info = this.infoPtr
            const id = v.getUint32(info, true)
            this.fn(this.params + 8)(this.plugin, id, this.valuePtr)
            const value = this.view.getFloat64(this.valuePtr, true)
            list.push({id, flags: v.getUint32(info + 4, true), name: this.cstr(info + 12), module: this.cstr(info + 268),
                min: v.getFloat64(info + 1296, true), max: v.getFloat64(info + 1304, true),
                defaultValue: v.getFloat64(info + 1312, true), value, text: this.valueText(id, value)})
        }
        return list
    }

    valueText(id, value) {
        if (!this.params) return ""
        return this.fn(this.params + 12)(this.plugin, id, value, this.textPtr, 256) ? this.cstr(this.textPtr) : ""
    }

    saveState() {
        if (!this.state) return null
        this.writeChunks = []
        if (!this.fn(this.state)(this.plugin, this.ostream)) return null
        const bytes = new Uint8Array(this.writeChunks.reduce((n, c) => n + c.length, 0))
        let offset = 0
        for (const c of this.writeChunks) { bytes.set(c, offset); offset += c.length }
        this.writeChunks = null
        return bytes.buffer
    }

    loadState(buffer) {
        if (!this.state) return
        this.readSource = {bytes: new Uint8Array(buffer), cursor: 0}
        this.fn(this.state + 4)(this.plugin, this.istream)
        this.readSource = null
    }

    openGui() {
        if (!this.gui || !this.webview) return this.port.postMessage({type: "gui", uri: null})
        const api = this.allocString("webview")
        if (!this.fn(this.gui + 8)(this.plugin, api, 0)) return this.port.postMessage({type: "gui", uri: null})
        const size = this.alloc(8)
        this.fn(this.gui + 20)(this.plugin, size, size + 4)
        const window = this.alloc(8)
        this.view.setUint32(window, api, true)
        this.view.setUint32(window + 4, 0, true)
        this.fn(this.gui + 40)(this.plugin, window)
        this.fn(this.gui + 52)(this.plugin)
        const length = this.fn(this.webview)(this.plugin, this.textPtr, 256)
        this.guiOpen = true
        this.port.postMessage({type: "gui", uri: length > 0 ? this.cstr(this.textPtr) : null,
            width: this.u32(size), height: this.u32(size + 4)})
    }

    closeGui() {
        if (!this.guiOpen) return
        this.guiOpen = false
        this.fn(this.gui + 56)(this.plugin)
        this.fn(this.gui + 12)(this.plugin)
    }

    // ---------------------------------------------------------------------------------------------------------
    // Events

    writeEvents() {
        const v = this.view
        const events = this.events.splice(0, MAX_EVENTS)
        events.forEach((e, i) => {
            const p = this.eventSlots + i * EVENT_SLOT
            new Uint8Array(this.memory.buffer, p, EVENT_SLOT).fill(0)
            v.setUint32(p + 4, 0, true)        // time
            v.setUint16(p + 8, 0, true)        // core space
            v.setUint16(p + 10, e.type, true)
            if (e.type === EVENT.PARAM_VALUE) {
                v.setUint32(p, 48, true)
                v.setUint32(p + 16, e.id, true)
                v.setInt32(p + 24, -1, true)
                v.setInt16(p + 28, -1, true); v.setInt16(p + 30, -1, true); v.setInt16(p + 32, -1, true)
                v.setFloat64(p + 40, e.value, true)
            } else if (e.type === EVENT.MIDI) {
                v.setUint32(p, 24, true)
                v.setUint16(p + 16, 0, true)
                e.data.slice(0, 3).forEach((b, k) => v.setUint8(p + 18 + k, b))
            } else {
                v.setUint32(p, 40, true)
                v.setInt32(p + 16, -1, true)
                v.setInt16(p + 20, 0, true); v.setInt16(p + 22, 0, true); v.setInt16(p + 24, e.key, true)
                v.setFloat64(p + 32, e.velocity, true)
            }
        })
        this.eventCount = events.length
    }

    outputEvent(ptr) {
        const v = this.view
        if (v.getUint16(ptr + 8, true) !== 0) return
        const type = v.getUint16(ptr + 10, true)
        if (type === EVENT.PARAM_VALUE) {
            const id = v.getUint32(ptr + 16, true), value = v.getFloat64(ptr + 40, true)
            this.port.postMessage({type: "paramOut", id, value, text: this.valueText(id, value)})
        } else if (type === EVENT.GESTURE_BEGIN || type === EVENT.GESTURE_END) {
            this.port.postMessage({type: "gesture", id: v.getUint32(ptr + 16, true), begin: type === EVENT.GESTURE_BEGIN})
        } else if (!this.noteEffect) {
            return
        } else if (type === EVENT.NOTE_ON || type === EVENT.NOTE_OFF || type === EVENT.NOTE_CHOKE) {
            this.notesOut.push({offset: v.getUint32(ptr + 4, true), on: type === EVENT.NOTE_ON, channel: v.getInt16(ptr + 22, true),
                key: v.getInt16(ptr + 24, true), velocity: v.getFloat64(ptr + 32, true)})
        } else if (type === EVENT.MIDI) {
            const midi = [0, 1, 2].map(index => v.getUint8(ptr + 18 + index))
            const status = midi[0] & 0xf0
            if (status === 0x90 || status === 0x80) {
                this.notesOut.push({offset: v.getUint32(ptr + 4, true), on: status === 0x90 && midi[2] > 0,
                    channel: midi[0] & 15, key: midi[1], velocity: midi[2] / 127})
            } else {
                this.notesOut.push({offset: v.getUint32(ptr + 4, true), midi})
            }
        }
    }

    // the note effect's output: sample exact on the sine, then to the page for the monitor and Web MIDI
    playNotesOut(left, right, time) {
        const events = this.notesOut.splice(0)
        if (left && right && this.playSine) {
            let position = 0
            for (const event of events) {
                const offset = Math.min(QUANTUM, event.offset)
                if (offset > position) { this.sine.render(left, right, position, offset); position = offset }
                if (event.key === undefined || event.key < 0) continue
                if (event.on) this.sine.start(event.key, event.velocity)
                else this.sine.stop(event.key)
            }
            this.sine.render(left, right, position, QUANTUM)
        }
        if (events.length > 0) this.port.postMessage({type: "notesOut", time: Number(time), events})
    }

    // Between process calls: flush if the plugin asked (so its output events arrive even while the audio is
    // suspended), and run on_main_thread if requested.
    afterMainThreadCall() {
        if (!this.ready) return
        if (this.flushRequested && this.params) {
            this.flushRequested = false
            this.writeEvents()
            this.fn(this.params + 20)(this.plugin, this.inEvents, this.outEvents)
            this.eventCount = 0
        }
        if (this.mainThreadRequested && this.honourCallbacks) {
            this.mainThreadRequested = false
            this.fn(this.plugin + 44)(this.plugin)
        }
    }

    // ---------------------------------------------------------------------------------------------------------

    process(inputs, outputs) {
        if (!this.ready) return true
        try {
            const v = this.view
            this.writeEvents()
            v.setBigUint64(this.processPtr, this.steadyTime, true)
            v.setUint32(this.processPtr + 8, QUANTUM, true)
            this.inputPorts.forEach((port, k) => {
                const source = inputs[k]
                port.channels.forEach((channel, ch) => {
                    const target = new Float32Array(this.memory.buffer, channel, QUANTUM)
                    if (source && source.length) target.set(source[Math.min(ch, source.length - 1)])
                    else target.fill(0)
                })
            })
            v.setUint32(this.processPtr + 24, this.inputPorts.length, true)
            v.setUint32(this.processPtr + 28, this.outputPorts.length, true)
            this.inProcess = true
            this.fn(this.plugin + 36)(this.plugin, this.processPtr)
            this.inProcess = false
            this.eventCount = 0
            this.flushRequested = false
            this.steadyTime += BigInt(QUANTUM)

            const out = outputs[0], main = this.outputPorts[0]
            if (main) for (let ch = 0; ch < out.length; ch++)
                out[ch].set(new Float32Array(this.memory.buffer, main.channels[Math.min(ch, main.channels.length - 1)], QUANTUM))
            if (this.noteEffect) this.playNotesOut(out[0], out[1] ?? out[0], this.steadyTime - BigInt(QUANTUM))

            if (this.mainThreadRequested && this.honourCallbacks) {
                this.mainThreadRequested = false
                this.fn(this.plugin + 44)(this.plugin)
            }
        } catch (error) {
            this.inProcess = false
            this.fail(error)
        }
        return true
    }
}

registerProcessor("clap-host", ClapHost)
