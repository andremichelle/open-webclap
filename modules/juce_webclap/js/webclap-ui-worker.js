// juce_webclap: the worker that runs ui.wasm and paints into the transferred OffscreenCanvas.
//
// The page drives it: one "frame" message per animation frame, and the next one only after this worker
// answered "frameDone", so a slow frame never queues up more work. Input arrives as small messages.
//
// Progress goes to the page as "progress" messages: the download of ui.wasm (phase "module", bytes), the files of a
// lazy folder fetched ahead (phase "prefetch", bytes), and each other file while it loads (phase "file", with the
// file's name and size; "fileDone" after).

"use strict"

let module = null
let canvas = null
let context = null
let imageData = null
let imageDataKey = ""
let pixelRatio = 1
const scrollLatchMs = 300
let scrollLatchUntil = 0 // page time (Event.timeStamp) until which wheel events scroll the host

const post = (message, transfer) => self.postMessage(message, transfer || [])

function framebufferImage() {
    const ptr = module._wclap_ui_framebuffer()
    const width = module._wclap_ui_framebuffer_width()
    const height = module._wclap_ui_framebuffer_height()
    const key = `${module.HEAPU8.buffer.byteLength}:${ptr}:${width}:${height}`
    if (key !== imageDataKey) { // memory grew or the desktop was resized: the old view is stale
        imageDataKey = key
        imageData = new ImageData(new Uint8ClampedArray(module.HEAPU8.buffer, ptr, width * height * 4), width, height)
        if (canvas.width !== width || canvas.height !== height) {
            canvas.width = width
            canvas.height = height
        }
    }
    return imageData
}

function frame(time) {
    const start = performance.now()
    const count = module._wclap_ui_frame(time)
    const ticked = performance.now()
    let pixels = 0
    if (count > 0) {
        const image = framebufferImage()
        const rects = module._wclap_ui_dirty_rects() >> 2
        for (let i = 0; i < count; i++) {
            const x = module.HEAP32[rects + i * 4], y = module.HEAP32[rects + i * 4 + 1]
            const w = module.HEAP32[rects + i * 4 + 2], h = module.HEAP32[rects + i * 4 + 3]
            context.putImageData(image, 0, 0, x, y, w, h)
            pixels += w * h
        }
    }
    const end = performance.now()
    post({type: "frameDone", stats: {tickMs: ticked - start, blitMs: end - ticked, rects: count, pixels}})
}

function withString(text, fn) {
    const ptr = module.stringToNewUTF8(text)
    try { fn(ptr) } finally { module._free(ptr) }
}

function receive(bytes) {
    const ptr = module._malloc(bytes.byteLength)
    module.HEAPU8.set(bytes, ptr)
    module._wclap_ui_receive(ptr, bytes.byteLength)
    module._free(ptr)
}

// A folder of bundle files (a plugin's factory data) in Emscripten's filesystem whose contents load when first read.
// The manifest lists every file with its size, so listings and sizes work without loading anything. A read loads
// the whole file with a synchronous request, which workers may make: the C++ side reads files synchronously.
async function fetchManifest(files) {
    const response = await fetch(files.manifest)
    if (!response.ok) throw new Error(`${files.manifest}: ${response.status}`)
    return response.json()
}

const lazyFolderUrl = (files, path) =>
    new URL(path.split("/").map(encodeURIComponent).join("/"), new URL(".", files.manifest)).href

// A dropped connection is retried (a busy server may refuse some of many parallel requests), an HTTP error is not
async function fetchBytes(url, attempts = 3) {
    for (let attempt = 1; ; attempt++) {
        let response
        try {
            response = await fetch(url)
        } catch (error) {
            if (attempt >= attempts) throw error
            await new Promise(resolve => setTimeout(resolve, 100 * attempt))
            continue
        }
        if (!response.ok) throw new Error(`${url}: ${response.status}`)
        return new Uint8Array(await response.arrayBuffer())
    }
}

// The files the plugin reads while it starts (files.prefetch: regular expressions on their paths), fetched in
// parallel with ui.wasm rather than one by one when read. Resolves to a Map of path to contents.
async function prefetchFiles(files, manifest) {
    const patterns = (files.prefetch || []).map(source => new RegExp(source))
    const wanted = manifest.files.filter(([path]) => patterns.some(pattern => pattern.test(path)))
    const total = wanted.reduce((sum, [, size]) => sum + size, 0)
    const contents = new Map()
    let loaded = 0, next = 0, lastPost = 0
    const worker = async () => {
        while (next < wanted.length) {
            const [path, size] = wanted[next++]
            contents.set(path, await fetchBytes(lazyFolderUrl(files, path)))
            loaded += size
            const now = performance.now()
            if (now - lastPost > 50 || loaded === total) {
                lastPost = now
                post({type: "progress", phase: "prefetch", loaded, total})
            }
        }
    }
    if (wanted.length > 0) post({type: "progress", phase: "prefetch", loaded: 0, total})
    await Promise.all(Array.from({length: 8}, worker))
    return contents
}

function mountLazyFolder(FS, files, manifest, prefetched) {
    const load = (node, path, size) => {
        if (node.contents !== null) return
        const ready = prefetched.get(path)
        if (ready) {
            node.contents = ready
            prefetched.delete(path)
            return
        }
        post({type: "progress", phase: "file", name: path.split("/").pop(), size})
        try {
            const request = new XMLHttpRequest()
            request.open("GET", lazyFolderUrl(files, path), false)
            request.responseType = "arraybuffer"
            request.send(null)
            if (request.status !== 200 && request.status !== 0) throw new Error(`${path}: ${request.status}`)
            node.contents = new Uint8Array(request.response)
        } catch (error) {
            post({type: "error", message: `Could not load ${path}: ${error.message || error}`})
            throw new FS.ErrnoError(29) // EIO
        } finally {
            post({type: "progress", phase: "fileDone"})
        }
    }
    for (const [path, size] of manifest.files) {
        const slash = path.lastIndexOf("/")
        const folder = files.mount + (slash >= 0 ? "/" + path.slice(0, slash) : "")
        FS.mkdirTree(folder)
        const node = FS.createFile(folder, path.slice(slash + 1), {}, true, false)
        node.contents = null
        Object.defineProperty(node, "usedBytes", {get() { return this.contents === null ? size : this.contents.length }})
        const ops = {}
        for (const [key, fn] of Object.entries(node.stream_ops))
            ops[key] = (stream, ...rest) => { load(stream.node, path, size); return fn(stream, ...rest) }
        node.stream_ops = ops
    }
}

// ui.wasm with download progress (its size from Content-Length, when the server sends it)
function instantiateWithProgress(wasmUrl) {
    return (imports, receiveInstance) => {
        (async () => {
            const response = await fetch(wasmUrl)
            if (!response.ok) throw new Error(`${wasmUrl}: ${response.status}`)
            const total = Number(response.headers.get("Content-Length")) || 0
            const reader = response.body.getReader()
            const chunks = []
            let loaded = 0, lastPost = 0
            for (;;) {
                const {done, value} = await reader.read()
                if (done) break
                chunks.push(value)
                loaded += value.byteLength
                const now = performance.now()
                if (now - lastPost > 50) {
                    lastPost = now
                    post({type: "progress", phase: "module", loaded, total})
                }
            }
            post({type: "progress", phase: "module", loaded, total: Math.max(total, loaded)})
            const bytes = new Uint8Array(loaded)
            let at = 0
            for (const chunk of chunks) { bytes.set(chunk, at); at += chunk.byteLength }
            const {instance, module} = await WebAssembly.instantiate(bytes, imports)
            receiveInstance(instance, module)
        })().catch(error => post({type: "error", message: String(error && error.stack || error)}))
        return {}
    }
}

async function init(message) {
    importScripts(message.moduleUrl)
    canvas = message.canvas
    context = canvas.getContext("2d", {alpha: false})
    pixelRatio = message.pixelRatio
    const host = {
        send: bytes => post({type: "send", bytes}, [bytes.buffer]),
        setCursor: name => post({type: "cursor", name}),
        requestSize: (width, height) => post({type: "requestSize", width, height}),
        copyText: text => post({type: "copyText", text}),
        textInput: (active, x, y) => post({type: "textInput", active, x, y}),
        toPage: text => post({type: "toPage", text})
    }
    const factory = self[message.factory]
    const manifest = message.files ? await fetchManifest(message.files) : null
    const prefetched = new Map()
    const prefetching = manifest ? prefetchFiles(message.files, manifest) : Promise.resolve(new Map())
    ;[module] = await Promise.all([
        factory({
            webclapHost: host,
            print: text => console.log(text),
            printErr: text => console.warn(text),
            instantiateWasm: instantiateWithProgress(message.moduleUrl.replace(/\.js$/, ".wasm")),
            preRun: manifest ? [m => mountLazyFolder(m.FS, message.files, manifest, prefetched)] : []
        }),
        prefetching.then(contents => contents.forEach((bytes, path) => prefetched.set(path, bytes)))
    ])
    const t0 = performance.now()
    module._wclap_ui_set_screen(message.screenWidth, message.screenHeight)
    if (typeof message.config === "string" && module._wclap_ui_configure)
        withString(message.config, ptr => module._wclap_ui_configure(ptr))
    if (!module._wclap_ui_init(pixelRatio)) throw new Error("wclap_ui_init failed")
    const initMs = performance.now() - t0
    const width = Math.round(module._wclap_ui_framebuffer_width() / pixelRatio)
    const height = Math.round(module._wclap_ui_framebuffer_height() / pixelRatio)
    let params = null
    if (module._wclap_ui_describe) {
        try { params = JSON.parse(module.UTF8ToString(module._wclap_ui_describe())) } catch (e) { params = null }
    }
    post({type: "ready", width, height, initMs, params})
}

self.onmessage = event => {
    const m = event.data
    try {
        switch (m.type) {
            case "init": init(m).catch(error => post({type: "error", message: String(error && error.stack || error)})); break
            case "frame":
                // Always answer, or the page stops sending frames after one failure.
                try { if (module) frame(m.time); else post({type: "frameDone", stats: null}) }
                catch (error) {
                    post({type: "frameDone", stats: null})
                    throw error
                }
                break
            case "mouse": module && module._wclap_ui_mouse(m.kind, m.x, m.y, m.buttons, m.modifiers); break
            case "wheel":
                // A gesture that started scrolling the host keeps scrolling it, also when a control moves under the
                // pointer (browsers latch scrolling the same way), until the wheel rests for scrollLatchMs. Decided
                // here, in event order and by the events' own times, however late the worker gets to them.
                if (m.time < scrollLatchUntil) {
                    scrollLatchUntil = m.time + scrollLatchMs
                    post({type: "unusedWheel", dom: m.dom})
                }
                // 0: no component used it (ports whose wclap_ui_wheel returns nothing never report that)
                else if (module && module._wclap_ui_wheel(m.x, m.y, m.dx, m.dy, m.smooth ? 1 : 0, m.modifiers) === 0) {
                    scrollLatchUntil = m.time + scrollLatchMs
                    post({type: "unusedWheel", dom: m.dom})
                }
                break
            case "key": module && module._wclap_ui_key(m.down ? 1 : 0, m.code, m.char, m.modifiers); break
            case "focus": module && module._wclap_ui_focus(m.focused ? 1 : 0); break
            case "clipboard": module && withString(m.text, ptr => module._wclap_ui_clipboard(ptr)); break
            case "screen": module && module._wclap_ui_set_screen(m.width, m.height); break
            case "desktop":
                pixelRatio = m.pixelRatio
                module && module._wclap_ui_set_desktop(m.width, m.height, m.pixelRatio)
                break
            case "receive": module && receive(new Uint8Array(m.bytes)); break
            case "pageInput":
                if (module && module._wclap_ui_page_input) {
                    const bytes = m.bytes instanceof Uint8Array ? m.bytes : new Uint8Array(m.bytes)
                    const ptr = module._malloc(bytes.byteLength)
                    module.HEAPU8.set(bytes, ptr)
                    module._wclap_ui_page_input(ptr, bytes.byteLength)
                    module._free(ptr)
                }
                break
            case "valueText":
                post({type: "valueText", id: m.id, value: m.value,
                    text: module ? module.UTF8ToString(module._wclap_ui_value_text(m.id, m.value)) : ""})
                break
        }
    } catch (error) {
        post({type: "error", message: String(error && error.stack || error)})
    }
}
