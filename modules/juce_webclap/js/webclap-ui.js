// juce_webclap: page glue for the plugin's web page (the WebCLAP webview).
//
// Starts the worker that runs ui.wasm, hands it an OffscreenCanvas, forwards input, and relays protocol
// frames between the worker and the host: out with window.parent.postMessage, in with "message" events.
//
//     startWebclapUI({canvas, moduleUrl: "ui.js", factory: "createWebclapUI"})
//
// Options: files: {manifest, mount, prefetch} mounts a folder of bundle files that load when first read, except the
// ones matching a prefetch pattern (regular expressions), which load with ui.wasm (see the worker);
// onProgress(m) gets the worker's progress messages: {phase: "module" or "prefetch", loaded, total} while ui.wasm and
// the prefetched files download, {phase: "file", name, size} while another file loads, then {phase: "fileDone"}.
// config: a string for the module's optional wclap_ui_configure, before wclap_ui_init (the page's mode, say);
// onPageMessage(text) gets what the module hands the page (host.toPage, a request only the page can serve, such as
// opening a file picker). The returned sendToModule(bytes) hands the module's optional wclap_ui_page_input bytes
// only the page has (a decoded file).

"use strict"

// KeyPress codes of the wasm platform (juce_wasm_Windowing.cpp): printable keys use their character,
// the rest the DOM keyCode ORed with an "extended" bit.
const EXTENDED = 0x10000
const SPECIAL_KEYS = {
    Enter: [0x0d, 0x0d], Tab: [0x09, 0x09], Escape: [0x1b, 0x1b], Backspace: [0x08, 0x08], " ": [0x20, 0x20],
    PageUp: [0x21 | EXTENDED, 0], PageDown: [0x22 | EXTENDED, 0], End: [0x23 | EXTENDED, 0],
    Home: [0x24 | EXTENDED, 0], ArrowLeft: [0x25 | EXTENDED, 0], ArrowUp: [0x26 | EXTENDED, 0],
    ArrowRight: [0x27 | EXTENDED, 0], ArrowDown: [0x28 | EXTENDED, 0], Insert: [0x2d | EXTENDED, 0],
    Delete: [0x2e | EXTENDED, 0x7f],
    MediaPlayPause: [0xb3 | EXTENDED, 0], MediaStop: [0xb2 | EXTENDED, 0],
    MediaTrackNext: [0xb0 | EXTENDED, 0], MediaTrackPrevious: [0xb1 | EXTENDED, 0]
}
const NUMPAD = {
    Numpad0: 0x60, Numpad1: 0x61, Numpad2: 0x62, Numpad3: 0x63, Numpad4: 0x64, Numpad5: 0x65, Numpad6: 0x66,
    Numpad7: 0x67, Numpad8: 0x68, Numpad9: 0x69, NumpadMultiply: 0x6a, NumpadAdd: 0x6b, NumpadComma: 0x6c,
    NumpadSubtract: 0x6d, NumpadDecimal: 0x6e, NumpadDivide: 0x6f, NumpadEqual: 0x93
}

function modifiersOf(e) {
    return (e.shiftKey ? 1 : 0) | (e.ctrlKey ? 2 : 0) | (e.altKey ? 4 : 0) | (e.metaKey ? 8 : 0)
}

function translateKey(e) {
    if (e.code in NUMPAD) {
        const ch = e.key.length === 1 ? e.key.codePointAt(0) : 0
        return [NUMPAD[e.code] | EXTENDED, ch]
    }
    if (e.key in SPECIAL_KEYS) return SPECIAL_KEYS[e.key]
    const f = /^F(\d{1,2})$/.exec(e.key)
    if (f && +f[1] >= 1 && +f[1] <= 35) return [(0x6f + +f[1]) | EXTENDED, 0]
    if ([...e.key].length === 1) {
        const ch = e.key.codePointAt(0)
        // With ctrl/cmd held the character is a shortcut, not text: JUCE expects the plain key code.
        const code = String.fromCodePoint(ch).toLowerCase().codePointAt(0)
        return [code, (e.ctrlKey || e.metaKey) ? 0 : ch]
    }
    return null // modifiers alone, dead keys, IME composition
}

// Keys the browser should keep, so a plugin window cannot trap the user. Undo and redo are the host's: a plugin's
// own history next to the host's would undo twice (hosts that forward keys from the window, like openDAW, see them).
function isBrowserShortcut(e) {
    if (e.key === "F5" || e.key === "F12") return true
    return (e.metaKey || e.ctrlKey) && /^[rwtlqnzy]$/i.test(e.key)
}

// Resolved while this script loads; document.currentScript is gone by the time startWebclapUI runs.
const WEBCLAP_UI_BASE = new URL(".", document.currentScript && document.currentScript.src || location.href)

function startWebclapUI({canvas, moduleUrl, factory, workerUrl, files, config, onStats, onReady, onError, onProgress,
                            onPageMessage}) {
    const base = WEBCLAP_UI_BASE
    const worker = new Worker(workerUrl || new URL("webclap-ui-worker.js", base))
    const offscreen = canvas.transferControlToOffscreen()
    let pixelRatio = window.devicePixelRatio || 1
    let logicalWidth = 0, logicalHeight = 0
    let frameInFlight = false, running = false
    const pendingFromHost = []

    const toHost = bytes => window.parent.postMessage(bytes.buffer, "*", [bytes.buffer])
    const post = (message, transfer) => worker.postMessage(message, transfer || [])

    function setSize(width, height) {
        logicalWidth = width
        logicalHeight = height
        canvas.style.width = `${width}px`
        canvas.style.height = `${height}px`
        post({type: "desktop", width, height, pixelRatio})
    }

    let lastFrameSent = 0
    function sendFrame(time) {
        if (!running || frameInFlight) return
        frameInFlight = true
        lastFrameSent = performance.now()
        post({type: "frame", time})
    }

    function loop(time) {
        if (!running) return
        sendFrame(time)
        requestAnimationFrame(loop)
    }

    // Browsers pause animation frames for hidden pages. Keep ticking slowly then, so protocol messages
    // (automation in, state out) and timers do not stall while the window is in the background.
    setInterval(() => {
        if (performance.now() - lastFrameSent > 200) sendFrame(performance.now())
    }, 100)

    worker.onmessage = event => {
        const m = event.data
        switch (m.type) {
            case "ready":
                running = true
                setSize(m.width, m.height)
                for (const bytes of pendingFromHost.splice(0)) post({type: "receive", bytes}, [bytes])
                requestAnimationFrame(loop)
                onReady && onReady(m)
                break
            case "frameDone":
                frameInFlight = false
                if (m.stats) onStats && onStats(m.stats)
                break
            case "send": toHost(m.bytes); break
            case "cursor": canvas.style.cursor = m.name; break
            case "requestSize": setSize(m.width, m.height); break
            // A wheel event no component used. The page cannot know that while the event is dispatched (the editor
            // runs in the worker), so it took it from the browser; hosts that show the window in a scrolling view
            // can scroll by it. Not part of WebCLAP: other hosts ignore the message.
            case "unusedWheel": window.parent.postMessage({webclapWheel: m.dom}, "*"); break
            case "copyText": navigator.clipboard && navigator.clipboard.writeText(m.text).catch(() => {}); break
            case "textInput": break // desktop keyboards deliver text through key events
            case "progress": onProgress && onProgress(m); break
            case "valueText": window.dispatchEvent(new CustomEvent("webclap-value-text", {detail: m})); break
            case "toPage": onPageMessage && onPageMessage(m.text); break
            case "error":
                console.error("[webclap-ui]", m.message)
                onError && onError(m.message)
                break
        }
    }

    // Protocol frames from the host (relayed DSP side)
    window.addEventListener("message", event => {
        if (event.source !== window.parent || !(event.data instanceof ArrayBuffer)) return
        if (running) post({type: "receive", bytes: event.data}, [event.data])
        else pendingFromHost.push(event.data)
    })

    // Pointer
    const position = e => {
        const r = canvas.getBoundingClientRect()
        return [(e.clientX - r.left) * logicalWidth / r.width, (e.clientY - r.top) * logicalHeight / r.height]
    }
    const mouse = (kind, e) => {
        const [x, y] = position(e)
        post({type: "mouse", kind, x, y, buttons: e.buttons, modifiers: modifiersOf(e)})
    }
    canvas.addEventListener("pointerdown", e => {
        canvas.focus()
        canvas.setPointerCapture(e.pointerId)
        mouse(1, e)
        e.preventDefault()
    })
    canvas.addEventListener("pointermove", e => mouse(0, e))
    canvas.addEventListener("pointerup", e => mouse(2, e))
    canvas.addEventListener("pointercancel", e => mouse(2, e))
    canvas.addEventListener("pointerleave", e => { if (e.buttons === 0) mouse(3, e) })
    canvas.addEventListener("contextmenu", e => e.preventDefault())
    canvas.addEventListener("wheel", e => {
        e.preventDefault()
        const dom = {deltaX: e.deltaX, deltaY: e.deltaY, deltaMode: e.deltaMode}
        const [x, y] = position(e)
        // JUCE units: about 0.2 per wheel notch, positive deltaY scrolls up.
        const scale = e.deltaMode === 1 ? 50 / 256 / 3 : e.deltaMode === 2 ? 1 : 0.5 / 256
        post({type: "wheel", x, y, dx: -e.deltaX * scale, dy: -e.deltaY * scale,
            smooth: e.deltaMode === 0, modifiers: modifiersOf(e), time: e.timeStamp, dom})
    }, {passive: false})

    // Keyboard
    canvas.tabIndex = 0
    const key = (down, e) => {
        if (e.isComposing || isBrowserShortcut(e)) return
        const translated = translateKey(e)
        if (translated) post({type: "key", down, code: translated[0], char: down ? translated[1] : 0, modifiers: modifiersOf(e)})
        e.preventDefault()
    }
    canvas.addEventListener("keydown", e => key(true, e))
    canvas.addEventListener("keyup", e => key(false, e))
    canvas.addEventListener("focus", () => post({type: "focus", focused: true}))
    canvas.addEventListener("blur", () => post({type: "focus", focused: false}))
    window.addEventListener("paste", e => post({type: "clipboard", text: e.clipboardData.getData("text/plain")}))

    // Device pixel ratio changes (moving between screens, browser zoom)
    const watchPixelRatio = () => {
        const query = matchMedia(`(resolution: ${pixelRatio}dppx)`)
        query.addEventListener("change", () => {
            pixelRatio = window.devicePixelRatio || 1
            if (running) setSize(logicalWidth, logicalHeight)
            watchPixelRatio()
        }, {once: true})
    }
    watchPixelRatio()

    post({type: "init", canvas: offscreen, moduleUrl: new URL(moduleUrl, location.href).href, factory, pixelRatio, config,
        screenWidth: screen.availWidth, screenHeight: screen.availHeight,
        files: files ? {manifest: new URL(files.manifest, location.href).href, mount: files.mount,
            prefetch: files.prefetch || []} : null}, [offscreen])

    return {
        worker,
        requestValueText: (id, value) => post({type: "valueText", id, value}),
        sendToModule: bytes => post({type: "pageInput", bytes}, [bytes.buffer])
    }
}
