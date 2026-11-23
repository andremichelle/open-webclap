// The page side of the openDAW WebCLAP kit (opendaw_kit.h): one card at 176 px for openDAW's device panel, the same
// card zoomed in the window. The plugin describes its parameters once, the page builds knobs, choices and rows from
// them (opendaw-page/panel.js, docs/device-panel-design.md) and polls the values, their texts and the meters at most
// every 33 ms. A plain script (no module), it defines `OpenDAWKit`.
//
//     OpenDAWKit.start({
//         layout: [{large: 0}, {knobs: [[1, "M"], [2, "M"]]}],   // optional, panel.js columns by parameter id
//         display: {width: 120, draw: (context, width, height, kit) => {...}}   // optional, kit.value(id), kit.meters
//     })
//
// What it keeps for openDAW: the hovered parameter is reported before any press (openDAW's right-click menu and
// double-click value entry), a drag is one gesture (one undo step), no wheel, no double-click and no right-click of
// its own, ctrl+click ignored, values in tabular digits at the width of their longest text, a value the user is
// changing is never overwritten by an older answer of the plugin.
"use strict"
var OpenDAWKit = (() => {
    const PAGE_DESCRIBE = 8, PAGE_SHOW_WINDOW = 9
    const PLUGIN_STATUS = 1, PLUGIN_DESCRIBE = 2
    const TEXT_BYTES = 16
    const POLL_MS = 33
    const ECHO_MS = 400
    const panelMode = new URLSearchParams(location.search).has("panel")

    // --- knob position (0..1) and value in its unit, through the parameter's curve ---------------------------------

    const positionOf = (param, value) => {
        const {min, max} = param
        const linear = (value - min) / (max - min)
        switch (param.curve) {
            case "exponential": return Math.log(value / min) / Math.log(max / min)
            case "squared": return Math.sqrt(Math.max(0, linear))
            default: return linear
        }
    }
    const valueOf = (param, position) => {
        const {min, max} = param
        const unit = Math.min(1, Math.max(0, position))
        switch (param.curve) {
            case "exponential": return min * Math.pow(max / min, unit)
            case "squared": return min + unit * unit * (max - min)
            default: return min + unit * (max - min)
        }
    }

    // --- the preview's report (only when the agent's local preview serves the page) ------------------------------

    const previewOrigin = (() => {
        try {
            const origin = window.top.location.origin
            const {hostname} = new URL(origin)
            return hostname === "localhost" || hostname === "127.0.0.1" ? origin : null
        } catch (_) {
            return null
        }
    })()
    const report = facts => {
        if (previewOrigin === null) {return}
        fetch(`${previewOrigin}/__local/report`, {method: "POST", body: JSON.stringify({panel: panelMode, ...facts})})
            .catch(() => {})
    }
    addEventListener("error", event => report({error: `${event.message} (${event.filename}:${event.lineno})`}))
    addEventListener("unhandledrejection", event => report({error: String(event.reason)}))

    // --- automatic layout after docs/device-panel-design.md -----------------------------------------------------

    // the main knob large and alone, the rest three to a column (four make a square), choices as rows, many minor
    // parameters as parameter rows; a single parameter beside a display goes into its corner
    const autoLayout = (params, hasDisplay) => {
        if (params.length === 1 && hasDisplay) {return {columns: [], corner: params[0].id}}
        const knobs = params.filter(param => param.kind !== "choice")
        const choices = params.filter(param => param.kind === "choice")
        const columns = []
        const asKnobs = knobs.slice(0, 7)
        const asRows = knobs.slice(7)
        if (asKnobs.length > 0) {
            const [main, ...rest] = asKnobs
            if (asKnobs.length >= 2 || !hasDisplay) {columns.push({large: main.id})}
            else {rest.unshift(main)}
            if (choices.length > 0 && choices.length <= 2 && rest.length > 0 && rest.length <= 2) {
                // a few: the choices with the knobs below them in one column, no knob alone in a column
                columns.push({stack: [...choices.map(param => [param.id, "M"]), ...rest.map(param => [param.id, "M"])]})
                choices.length = 0
            } else if (rest.length === 4) {
                // four make a square: two columns of two
                columns.push({knobs: rest.slice(0, 2).map(param => [param.id, "M"]), rows: 2})
                columns.push({knobs: rest.slice(2).map(param => [param.id, "M"]), rows: 2})
            } else {
                for (let index = 0; index < rest.length; index += 3) {
                    columns.push({knobs: rest.slice(index, index + 3).map(param => [param.id, "M"])})
                }
            }
        }
        if (choices.length > 0) {columns.push({choices: choices.map(param => param.id)})}
        if (asRows.length > 0) {columns.push({labels: asRows.map(param => param.id)})}
        return {columns, corner: null}
    }

    // --- the plugin ----------------------------------------------------------------------------------------------

    const post = bytes => window.parent.postMessage(bytes, "*")
    const describe = () => post(new Uint8Array([PAGE_DESCRIBE]).buffer)
    const showWindow = () => post(new Uint8Array([PAGE_SHOW_WINDOW]).buffer)

    // the kit's own pieces, after panel.css: the panel's button that opens the window, the window's area
    const style = document.createElement("style")
    style.textContent = `
        .kit-open {height: 13px; line-height: 13px; padding: 0 6px; border-radius: 2px; font-size: 9px; color: var(--step);
            background: rgba(255, 255, 255, 0.04); cursor: pointer; white-space: nowrap; margin: auto 0}
        .kit-open:hover {color: var(--value)}
        .kit-window {position: relative; overflow: hidden; background: var(--page)}`
    document.head.append(style)

    const start = (options = {}) => {
        const stage = document.getElementById("stage") ?? document.body.appendChild(Object.assign(
            document.createElement("div"), {id: "stage"}))
        const waiting = Object.assign(document.createElement("div"), {className: "kit-waiting", textContent: "Loading…"})
        stage.append(waiting)
        let card = null
        // ask until the plugin answers (it may still be loading)
        const asking = setInterval(describe, 250)
        describe()
        addEventListener("message", event => {
            if (!(event.data instanceof ArrayBuffer) || event.data.byteLength < 1) {return}
            const view = new DataView(event.data)
            const type = view.getUint8(0)
            if (type === PLUGIN_DESCRIBE && card === null) {
                let description = null
                try {
                    description = JSON.parse(new TextDecoder().decode(new Uint8Array(event.data, 1)))
                } catch (error) {
                    report({error: `the plugin's description is no JSON: ${error}`})
                    return
                }
                clearInterval(asking)
                waiting.remove()
                card = build(stage, description, options)
            } else if (type === PLUGIN_STATUS && card !== null) {
                card.status(view)
            }
        })
    }

    const build = (stage, description, options) => {
        const host = OpenDAWPanel.pluginHost()
        const params = description.params
        const byId = new Map(params.map(param => [param.id, param]))
        const values = new Map(params.map(param => [param.id, param.value]))
        const texts = new Map(params.map(param => [param.id, ""]))
        // the value this page sent last and when: the plugin's older answers do not overwrite it
        const sent = new Map()
        const meters = []
        const frames = []
        // the window shows content of its own (options.window), the device panel the card
        const windowed = !panelMode && description.window !== null && options.window !== undefined
        if (!panelMode && description.window !== null && options.window === undefined) {
            report({error: "the plugin has a window (windowWidth in INFO) but the page builds none: give OpenDAWKit.start a window, or remove the window"})
        }
        if (description.window === null && options.window !== undefined) {
            report({error: "the page builds a window but the plugin has none: set windowWidth and windowHeight in INFO"})
        }
        const display = windowed ? null : options.display ?? null
        const element = document.createElement("div")
        element.className = "card"
        element.style.width = `${description.width}px`
        const screen = display === null ? null : Object.assign(document.createElement("div"), {className: "screen"})
        const canvas = display === null ? null : document.createElement("canvas")
        if (screen !== null) {
            screen.style.width = `${display.width}px`
            screen.append(canvas)
            element.append(screen)
        }
        const group = Object.assign(document.createElement("div"), {className: "group"})
        element.append(group)
        stage.append(element)
        // panel.js works on knob positions (0..1) for "range", whole numbers for "int" and steps for "steps"
        const panelParam = param => ({
            id: param.id, label: param.name,
            kind: param.kind === "knob" ? "range" : param.kind === "integer" ? "int" : "steps",
            low: param.min, high: param.max, labels: param.labels,
            bipolar: param.kind !== "choice" && param.min < 0 && param.max > 0 && Math.abs(param.min + param.max) < 1e-9
        })
        const panelParams = new Map(params.map(param => [param.id, panelParam(param)]))
        const controls = OpenDAWPanel.controls({
            param: id => panelParams.get(id),
            get: id => {
                const param = byId.get(id)
                return param.kind === "knob" ? positionOf(param, values.get(id)) : values.get(id)
            },
            set: (id, next) => {
                const param = byId.get(id)
                const value = param.kind === "knob" ? valueOf(param, next) : Math.round(next)
                if (value === values.get(id)) {return}
                values.set(id, value)
                sent.set(id, {value, time: performance.now()})
                host.set(id, value)
                controls.update()
            },
            // the plugin's own text for the current value; any other value only for the readout's width
            text: (id, value) => {
                const param = byId.get(id)
                const current = param.kind === "knob" ? positionOf(param, values.get(id)) : values.get(id)
                return Math.abs(value - current) < 1e-12 && texts.get(id) !== "" ? texts.get(id) : "0".repeat(param.chars)
            },
            host
        })
        const kit = {
            value: id => values.get(id),
            text: id => texts.get(id),
            meters,
            time: 0,
            zoom: () => zoom,
            showWindow,
            panel: panelMode,
            // for the window's own content: openDAW's controls for any parameter, laid out or alone
            control: (id, size = "M") => controls.control(id, OpenDAWPanel.SIZES[size]),
            row: id => controls.row(id),
            layout: (columns, container) => controls.layout(columns, container),
            onFrame: callback => frames.push(callback)
        }
        let zoom = 1
        if (windowed) {
            // the window: its own size, zoomed to the host's window, the plugin's content in it
            element.remove()
            const {width, height} = description.window
            const root = Object.assign(document.createElement("div"), {className: "kit-window"})
            root.style.width = `${width}px`
            root.style.height = `${height}px`
            stage.append(root)
            options.window.build(root, kit)
            const resize = () => {
                zoom = Math.max(0.25, Math.min(innerWidth / width, innerHeight / height))
                stage.style.transform = `scale(${zoom})`
            }
            addEventListener("resize", resize)
            resize()
            controls.update()
        } else {
            const {columns, corner} = options.layout !== undefined
                ? {columns: options.layout, corner: options.corner ?? null}
                : autoLayout(params, display !== null)
            controls.layout(columns, group)
            if (corner !== null && screen !== null) {controls.corner(corner, screen)}
            // a plugin with a window opens it from the panel: in the display's corner, or as the last column
            if (description.window !== null && panelMode) {
                const open = Object.assign(document.createElement("div"), {textContent: "Big View"})
                open.addEventListener("click", showWindow)
                if (screen !== null) {
                    open.className = "edit"
                    screen.append(open)
                } else {
                    open.className = "kit-open"
                    const column = Object.assign(document.createElement("div"), {className: "column stack"})
                    column.append(open)
                    group.append(column)
                }
            }
            controls.update()
            OpenDAWPanel.zoomToWindow(stage, description.width, value => {zoom = value})
        }
        // the content's own width against the declared one, for the preview's report
        document.fonts.ready.then(() => {
            if (windowed) {return}
            element.style.width = "max-content"
            const content = Math.ceil(element.getBoundingClientRect().width / zoom)
            element.style.width = `${description.width}px`
            if (content > description.width) {
                console.warn(`${description.name}: the panel content is ${content} px, the plugin declares ${description.width} px`)
            }
            report({declaredWidth: description.width, contentWidth: content, params: params.length})
        })
        const status = view => {
            if (view.byteLength < 8) {return}
            const count = view.getUint16(2, true), meterCount = view.getUint16(4, true)
            if (count !== params.length || view.byteLength < 8 + count * (8 + TEXT_BYTES) + meterCount * 4) {return}
            const now = performance.now()
            let changed = false
            for (let index = 0; index < count; index++) {
                const id = params[index].id
                const text = new TextDecoder().decode(new Uint8Array(view.buffer, 8 + count * 8 + index * TEXT_BYTES, TEXT_BYTES))
                    .replace(/\0+$/, "")
                if (text !== texts.get(id)) {
                    texts.set(id, text)
                    changed = true
                }
                if (controls.dragging.has(id)) {continue}
                const value = view.getFloat64(8 + index * 8, true)
                const mine = sent.get(id)
                if (mine !== undefined) {
                    if (value !== mine.value && now - mine.time < ECHO_MS) {continue}
                    sent.delete(id)
                }
                if (value !== values.get(id)) {
                    values.set(id, value)
                    changed = true
                }
            }
            meters.length = meterCount
            const offset = 8 + count * (8 + TEXT_BYTES)
            for (let index = 0; index < meterCount; index++) {meters[index] = view.getFloat32(offset + index * 4, true)}
            if (changed) {controls.update()}
        }
        let lastPoll = 0
        const context = canvas?.getContext("2d") ?? null
        const frame = now => {
            if (now - lastPoll >= POLL_MS) {
                lastPoll = now
                host.poll()
            }
            kit.time = now / 1000
            if (display !== null && context !== null) {
                const {width, height} = OpenDAWPanel.fit(canvas, context, zoom)
                display.draw(context, width, height, kit)
            }
            frames.forEach(callback => callback(kit))
            requestAnimationFrame(frame)
        }
        document.fonts.ready.then(() => requestAnimationFrame(frame))
        return {status}
    }

    return {start, positionOf, valueOf, panel: panelMode}
})()
