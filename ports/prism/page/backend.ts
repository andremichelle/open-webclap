// The WebCLAP stand-in for JUCE's webview backend (window.__JUCE__.backend), which Prism's plugin UI talks to
// through juceBridge.ts: its events become binary messages to the plugin (plugin/prism.cpp), the plugin's frames
// become the events the UI listens to, with Float32Arrays where JUCE sends base64 (patches/prism-webclap.patch lets
// juceBridge take either).
//
// Every page has an id of its own and an engine of its own in the plugin (a host may show the device panel's page
// more than once), and every page receives every message. There is no window: upstream's settings panel below the
// scope (prismSettingsPanel, a resize) does not happen, the device panel shows the settings beside the scope.

const SCOPES = ["spectrum", "oscilloscope", "vectorscope", "spectrogram", "vumeter", "lufsmeter", "waveform", "waterfall"]
const FRAME_EVENTS = SCOPES.map(scope => `${scope}Frame`)

// page -> plugin (prism.cpp): the type at 0, the page at 4
const PAGE_POLL = 1, PAGE_READY = 2, PAGE_CONFIG = 3, PAGE_BYE = 5
const CONFIG_SETTINGS = 0, CONFIG_NATIVE = 1, CONFIG_MEASUREMENT = 2
// plugin -> page
const PLUGIN_FRAME = 1, PLUGIN_RESTORE = 2
const FRAME_HEADER = 8

export const scope: string = SCOPES.includes(location.hash.slice(1)) ? location.hash.slice(1) : "spectrum"
export const panel: boolean = new URLSearchParams(location.search).has("panel")
const page = (crypto.getRandomValues(new Uint32Array(1))[0] | 1) >>> 0
const hosted = window.parent !== window

type Listener = (payload: unknown) => void
const listeners = new Map<number, {event: string, listener: Listener}>()
let nextListener = 1

const dispatch = (event: string, payload: unknown): void => {
    for (const entry of [...listeners.values()]) {if (entry.event === event) {entry.listener(payload)}}
}

const post = (bytes: Uint8Array): void => {
    if (hosted) {window.parent.postMessage(bytes.buffer, "*")}
}

const message = (type: number, extra: number = 0): Uint8Array => {
    const bytes = new Uint8Array(8 + extra)
    const view = new DataView(bytes.buffer)
    view.setUint8(0, type)
    view.setUint32(4, page, true)
    return bytes
}

const encoder = new TextEncoder()
const sendConfig = (kind: number, payload: unknown, persist: boolean = false): void => {
    const json = encoder.encode(JSON.stringify(payload ?? {}))
    const bytes = message(PAGE_CONFIG, 4 + json.length)
    bytes[8] = kind
    bytes[9] = persist ? 1 : 0
    bytes.set(json, 12)
    post(bytes)
}

// the plugin answers a poll with a frame; one poll at a time, at most one a display frame
let polling = false
let pending = false
let lastPoll = 0
const poll = (time: number): void => {
    requestAnimationFrame(poll)
    if (pending && time - lastPoll < 250) {return}
    pending = true
    lastPoll = time
    post(message(PAGE_POLL))
}

// a frame's fields after the header (engines.h): 4 bytes each, an array as its count and floats
class Reader {
    private at = FRAME_HEADER
    constructor(private readonly view: DataView) {}
    f32(): number {const value = this.view.getFloat32(this.at, true); this.at += 4; return value}
    u32(): number {const value = this.view.getUint32(this.at, true); this.at += 4; return value}
    floats(): Float32Array {
        const count = this.u32()
        const values = new Float32Array(this.view.buffer.slice(this.at, this.at + count * 4))
        this.at += count * 4
        return values
    }
}

// the frame as the JUCE plugin's payload (plugin/Source/*Engine.h buildFrame), arrays as Float32Arrays
const decodeFrame = (kind: number, read: Reader): Record<string, unknown> => {
    switch (SCOPES[kind]) {
        case "spectrum": return {
            sampleRate: read.f32(), hasSpectrumData: read.u32() !== 0, referenceEnabled: read.u32() !== 0,
            referenceMeanSquare: read.f32(), referenceSeconds: read.f32(),
            magnitudes: read.floats(), side: read.floats(), channelMax: read.floats()
        }
        case "oscilloscope": return {sampleRate: read.f32(), pitch: read.f32(), samples: read.floats()}
        case "vectorscope": {
            const sampleRate = read.f32(), multiband = read.u32() !== 0, count = read.u32()
            return multiband ? {sampleRate, multiband, count, data: read.floats()}
                : {sampleRate, multiband, count, x: read.floats(), y: read.floats()}
        }
        case "spectrogram": return {
            sampleRate: read.f32(), rowCount: read.u32(), columnCount: read.u32(), display: read.floats(), heat: read.floats()
        }
        case "vumeter": return {
            sampleRate: read.f32(), vuLDb: read.f32(), vuRDb: read.f32(), barLDb: read.f32(), barRDb: read.f32(),
            peakLDb: read.f32(), peakRDb: read.f32(), correlation: read.f32()
        }
        case "lufsmeter": return {
            sampleRate: read.f32(), momentaryLUFS: read.f32(), shortTermLUFS: read.f32(), integratedLUFS: read.f32(),
            vuLDb: read.f32(), vuRDb: read.f32(), barLDb: read.f32(), barRDb: read.f32(),
            truePeakLDb: read.f32(), truePeakRDb: read.f32(), maxTruePeakDb: read.f32(), correlation: read.f32()
        }
        case "waveform": return {sampleRate: read.f32(), stereo: read.u32() !== 0, columnCount: read.u32(), summaries: read.floats()}
        default: return {
            sampleRate: read.f32(), revision: read.u32(), columns: read.u32(), audioSeconds: read.f32(),
            levels: read.floats(), ages: read.floats(), frequencies: read.floats()
        }
    }
}

const decoder = new TextDecoder()
addEventListener("message", event => {
    if (!(event.data instanceof ArrayBuffer) || event.data.byteLength < 8) {return}
    const view = new DataView(event.data)
    const target = view.getUint32(4, true)
    switch (view.getUint8(0)) {
        case PLUGIN_FRAME: {
            if (target !== page) {return}
            pending = false
            const kind = view.getUint8(1)
            if (kind >= SCOPES.length) {return}
            try {
                dispatch(FRAME_EVENTS[kind], decodeFrame(kind, new Reader(view)))
            } catch (error) {
                console.warn("[prism] bad frame", error)
            }
            return
        }
        case PLUGIN_RESTORE: {
            if (view.byteLength < 12 || (target !== 0 && target !== page) || view.getUint32(8, true) === page) {return}
            const json = decoder.decode(new Uint8Array(event.data, 12))
            if (json.length > 0) {dispatch("prismRestoreSettings", {json})}
            return
        }
    }
})

// Prism's reference tracks (Spectrum settings > Reference) read the file in the plugin, not yet in this build
const referenceCommand = (payload: Record<string, unknown>): void => {
    const response: Record<string, unknown> = {requestId: payload.requestId}
    if (payload.action === "getState") {
        response.ok = true
        response.state = {jobId: null, phase: "idle", name: "", progress: null, preview: null, result: null, error: null}
    } else if (payload.action === "cancel" || payload.action === "cancelUpload") {
        response.ok = true
    } else {
        response.ok = false
        response.error = "Reference tracks are not available in the WebCLAP build."
    }
    setTimeout(() => dispatch("prismReferenceResponse", response), 0)
}

const backend = {
    addEventListener(event: string, listener: Listener): number {
        const id = nextListener++
        listeners.set(id, {event, listener})
        return id
    },
    removeEventListener(id: number): void {listeners.delete(id)},
    emitEvent(event: string, payload: unknown): void {
        const value = (payload ?? {}) as Record<string, unknown>
        switch (event) {
            case "prismReady":
                post(message(PAGE_READY))
                if (!polling) {
                    polling = true
                    requestAnimationFrame(poll)
                }
                return
            case "prismConfig":
                sendConfig(CONFIG_SETTINGS, value.settings, value.persist === true)
                return
            case "prismSpectrogramConfig":
            case "prismWaterfallConfig":
            case "prismSpectrumReferenceConfig":
                sendConfig(CONFIG_NATIVE, value)
                return
            case "prismScopeMeasurement":
                sendConfig(CONFIG_MEASUREMENT, value)
                return
            case "prismReferenceCommand":
                referenceCommand(value)
                return
        }
    }
}

addEventListener("pagehide", () => post(message(PAGE_BYE)))

// Space is openDAW's playback. A control with the focus would take it too (a button or chip clicks, a dropdown opens),
// and the frame does not forward keys typed into an input (the sliders) at all. So the controls give the focus back
// once used, and space never acts on one that still has it (reached with Tab).
const isControl = (element: Element | null): element is HTMLElement =>
    element instanceof HTMLButtonElement || element instanceof HTMLInputElement || element instanceof HTMLSelectElement
addEventListener("pointerup", () => setTimeout(() => {
    const active = document.activeElement
    // a dropdown keeps it while its list is open, it gives it back on its change
    if (isControl(active) && !(active instanceof HTMLSelectElement)) {active.blur()}
}, 0), {capture: true})
addEventListener("change", event => {
    if (isControl(event.target as Element)) {(event.target as HTMLElement).blur()}
}, {capture: true})
for (const kind of ["keydown", "keyup"] as const) {
    addEventListener(kind, event => {
        if (event.key !== " " || !isControl(event.target as Element)) {return}
        event.preventDefault()
        if (kind === "keyup") {(event.target as HTMLElement).blur()}
    }, {capture: true})
}

// without a host (the page opened on its own), juceBridge's synthetic signals drive the UI
if (hosted) {
    window.__JUCE__ = {backend, initialisationData: {prismScope: [scope]}}
} else {
    window.__JUCE__ = {initialisationData: {prismScope: [scope]}}
}
document.documentElement.classList.toggle("prism-panel", panel)
document.documentElement.dataset.scope = scope
