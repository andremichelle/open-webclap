// openDAW device panel controls, after docs/device-panel-design.md: knobs in three sizes, choices, parameter rows, the
// layout of a card's controls, the drawing kit for displays and the bridge to the plugin. A plain script (no module),
// so a page opened from file:// works too; it defines `OpenDAWPanel`. A port copies this folder next to its page.
//
// Controls work on parameters the page describes: {id, label, kind, low, high, labels, bipolar}. Kinds: "range" a
// knob position 0..1; "int" a whole number from low to high, on a knob (Gate 0..100, Transpose -24..24); "steps" a
// choice from low to high, its labels shown. bipolar: the knob's arc starts at the top. The page keeps the values;
// the controls read and write them through the context they are made with.
"use strict"
var OpenDAWPanel = (() => {
    const SIZES = {L: 44, M: 28, S: 20}
    const clamp = (value, low, high) => Math.min(high, Math.max(low, value))

    // --- knob ---------------------------------------------------------------------------------------------------

    const SVG = "http://www.w3.org/2000/svg"
    const svg = (name, attributes = {}, parent) => {
        const element = document.createElementNS(SVG, name)
        for (const [key, value] of Object.entries(attributes)) {element.setAttribute(key, value)}
        parent?.append(element)
        return element
    }
    const START = -135, SWEEP = 270
    const polar = (cx, cy, radius, degrees) => {
        const radians = degrees * Math.PI / 180
        return [cx + radius * Math.sin(radians), cy - radius * Math.cos(radians)]
    }
    const arc = (cx, cy, radius, from, to) => {
        if (Math.abs(to - from) < 0.01) {return ""}
        const [x0, y0] = polar(cx, cy, radius, Math.min(from, to))
        const [x1, y1] = polar(cx, cy, radius, Math.max(from, to))
        const large = Math.abs(to - from) > 180 ? 1 : 0
        return `M${x0.toFixed(2)} ${y0.toFixed(2)}A${radius} ${radius} 0 ${large} 1 ${x1.toFixed(2)} ${y1.toFixed(2)}`
    }
    // the studio's proportions: a 1.5 px track, faint grey, the value over it and the pointer in the label colour, a
    // dark cap inside
    const knobView = (size, bipolar) => {
        const c = size / 2, ring = c - 0.75, cap = c - 4
        const root = svg("svg", {class: "knob", width: size, height: size, viewBox: `0 0 ${size} ${size}`})
        svg("path", {d: arc(c, c, ring, START, START + SWEEP), fill: "none", stroke: "var(--track)", "stroke-opacity": 0.14,
            "stroke-width": 1.5}, root)
        const value = svg("path", {fill: "none", stroke: "var(--label)", "stroke-width": 1.5}, root)
        svg("circle", {cx: c, cy: c, r: cap, fill: "var(--knob)"}, root)
        const pointer = svg("line", {x1: c, y1: c - cap * 0.35, x2: c, y2: c - cap + 1.25, stroke: "var(--label)",
            "stroke-width": 1.25, "stroke-linecap": "round"}, root)
        return {
            element: root,
            update: unit => {
                const angle = START + unit * SWEEP
                value.setAttribute("d", bipolar ? arc(c, c, ring, 0, angle) : arc(c, c, ring, START, angle))
                pointer.setAttribute("transform", `rotate(${angle} ${c} ${c})`)
            }
        }
    }

    // --- controls -----------------------------------------------------------------------------------------------

    // The context: param(key) the description, get(key) the value, set(key, value) to change it (the page stores it,
    // sends it and calls update()), text(key, value) the value as shown, host {begin, end, hover} with the parameter's
    // id (hover null: none). Each control registers a view; update() redraws them all.
    const controls = context => {
        const {param, get, set, text, host} = context
        const views = []
        const dragging = new Set()
        const update = () => views.forEach(view => view())
        // a value as a knob position and back
        const unitOf = (p, value) => p.kind === "range" ? value : (value - p.low) / (p.high - p.low)
        const valueOf = (p, unit) => p.kind === "range" ? unit : p.low + Math.round(unit * (p.high - p.low))
        // every value a parameter can show, for the width of its readout
        const allValues = p => p.kind === "range" ? Array.from({length: 101}, (_, index) => index / 100)
            : Array.from({length: Math.min(p.high - p.low, 1000) + 1}, (_, index) => p.low + index)

        // a vertical drag on the element changes a range parameter, one gesture; Shift or Alt for fine steps;
        // hovering reports the parameter, so openDAW's menu and value entry work on it
        const draggable = (element, key, onStep) => {
            const p = param(key)
            let drag = null
            element.addEventListener("pointerdown", () => host.hover(p.id), {capture: true})
            element.addEventListener("pointerdown", event => {
                if (event.button !== 0 || event.ctrlKey || drag !== null) {return}
                if (p.kind === "steps") {
                    onStep?.()
                    return
                }
                element.setPointerCapture(event.pointerId)
                drag = {pointerId: event.pointerId, unit: unitOf(p, get(key))}
                element.classList.add("active")
                dragging.add(key)
                host.begin(p.id)
            })
            element.addEventListener("pointermove", event => {
                host.hover(p.id)
                if (drag === null || event.pointerId !== drag.pointerId) {return}
                drag.unit = clamp(drag.unit - event.movementY / 128 * (event.altKey || event.shiftKey ? 0.2 : 1.2), 0, 1)
                const value = valueOf(p, drag.unit)
                if (value !== get(key)) {set(key, value)}
            })
            const end = () => {
                if (drag === null) {return}
                drag = null
                element.classList.remove("active")
                dragging.delete(key)
                host.end(p.id)
            }
            element.addEventListener("pointerup", end)
            element.addEventListener("lostpointercapture", end)
            element.addEventListener("pointerenter", () => {
                host.hover(p.id)
                element.classList.add("active")
            })
            element.addEventListener("pointerleave", () => {
                if (drag === null) {
                    element.classList.remove("active")
                    host.hover(null)
                }
            })
        }
        // a click selects a step, one gesture
        const step = (key, value) => {
            const p = param(key)
            host.begin(p.id)
            set(key, clamp(Math.round(value), p.low, p.high))
            host.end(p.id)
        }

        // a knob: its label below, the value in its place while hovered or dragged
        const knob = (key, size = SIZES.M) => {
            const p = param(key)
            const element = document.createElement("div")
            element.className = "control"
            const view = knobView(size, p.bipolar === true)
            const label = Object.assign(document.createElement("div"), {className: "label", textContent: p.label})
            const readout = Object.assign(document.createElement("div"), {className: "readout"})
            element.append(view.element, label, readout)
            views.push(() => {
                view.update(unitOf(p, get(key)))
                readout.textContent = text(key, get(key))
                element.classList.toggle("inactive", p.isActive?.() === false)
            })
            draggable(element, key)
            return element
        }

        // a choice: its steps as words, a click selects one; no name when the steps say what it does (label "")
        const choice = key => {
            const p = param(key)
            const element = document.createElement("div")
            element.className = "choice"
            element.innerHTML = `<div class="pills"></div>`
            if (p.label !== "") {element.append(Object.assign(document.createElement("div"), {className: "label", textContent: p.label}))}
            const pills = p.labels.map((label, index) => {
                const pill = Object.assign(document.createElement("span"), {textContent: label})
                pill.addEventListener("pointerdown", event => {
                    if (event.button === 0 && !event.ctrlKey) {step(key, p.low + index)}
                })
                element.querySelector(".pills").append(pill)
                return pill
            })
            element.addEventListener("pointerdown", () => host.hover(p.id), {capture: true})
            element.addEventListener("pointermove", () => host.hover(p.id))
            element.addEventListener("pointerleave", () => host.hover(null))
            views.push(() => pills.forEach((pill, index) => pill.classList.toggle("on", index === Math.round(get(key) - p.low))))
            return element
        }

        // a parameter row: name and value on one line, dragged like a knob, a choice steps on a click; the value as
        // wide as the widest text it can show, so a change never moves anything
        const row = key => {
            const p = param(key)
            const element = document.createElement("div")
            element.className = "parameter"
            element.innerHTML = `<span class="name"></span><span class="value"></span>`
            element.querySelector(".name").textContent = p.label
            const value = element.querySelector(".value")
            // texts may come later (a kit plugin's, see kitHost): the width follows them
            const widest = () => Math.max(0, ...allValues(p).map(value => text(key, value).length))
            views.push(() => {
                value.style.minWidth = `${widest()}ch`
                value.textContent = text(key, get(key))
            })
            draggable(element, key, () => step(key, get(key) >= p.high ? p.low : get(key) + 1))
            return element
        }

        // a choice as a row: the name, then its steps on the line, the current one lit; a click selects a step (as the
        // plugin's own editor shows a choice: "Play  Note Free")
        const choiceRow = key => {
            const p = param(key)
            const element = document.createElement("div")
            element.className = "parameter steps"
            element.innerHTML = `<span class="name"></span><span class="pills"></span>`
            element.querySelector(".name").textContent = p.label
            const pills = p.labels.map((label, index) => {
                const pill = Object.assign(document.createElement("span"), {textContent: label})
                pill.addEventListener("pointerdown", event => {
                    if (event.button === 0 && !event.ctrlKey) {step(key, p.low + index)}
                })
                element.querySelector(".pills").append(pill)
                return pill
            })
            element.addEventListener("pointerdown", () => host.hover(p.id), {capture: true})
            element.addEventListener("pointermove", () => host.hover(p.id))
            element.addEventListener("pointerleave", () => host.hover(null))
            views.push(() => pills.forEach((pill, index) => pill.classList.toggle("on", index === Math.round(get(key) - p.low))))
            return element
        }

        // a stepped parameter is a choice, any other a knob
        const control = (key, size) => param(key).kind === "steps" ? choice(key) : knob(key, size)

        // The columns of a card's controls, left to right: {large: key} the main knob alone, centred by its circle;
        // {knobs: [[key, size] | null, ...], rows} up to three per column (rows 2: a square of four); {stack: [[key,
        // size], ...]} top to bottom; {labels: [key | null, ...]} parameter rows, null a gap between groups; {choices:
        // [key, ...], square} choices as rows together, every step on the line; with square ([[key, size] x 4], row
        // by row) the choices on the first row and the knobs as a square on the other two.
        const layout = (columns, group) => {
            for (const column of columns) {
                const cells = document.createElement("div")
                if (column.large !== undefined) {
                    cells.className = "column large"
                    cells.append(control(column.large, SIZES.L))
                } else if (column.knobs !== undefined) {
                    cells.className = `column rows-${column.rows ?? 3}`
                    for (const cell of column.knobs) {
                        const slot = Object.assign(document.createElement("div"), {className: "cell"})
                        if (cell !== null) {slot.append(control(cell[0], SIZES[cell[1]]))}
                        cells.append(slot)
                    }
                } else if (column.stack !== undefined) {
                    cells.className = "column stack"
                    for (const [key, size] of column.stack) {cells.append(control(key, SIZES[size]))}
                } else if (column.choices !== undefined && column.square !== undefined) {
                    // the choices together on the first row, a square of four knobs on the other two
                    cells.className = "column choices-over"
                    const rows = Object.assign(document.createElement("div"), {className: "column choices"})
                    for (const key of column.choices) {rows.append(choiceRow(key))}
                    cells.append(rows)
                    for (const [key, size] of column.square) {
                        const slot = Object.assign(document.createElement("div"), {className: "cell"})
                        slot.append(control(key, SIZES[size]))
                        cells.append(slot)
                    }
                } else if (column.choices !== undefined) {
                    cells.className = "column labels choices"
                    for (const key of column.choices) {cells.append(choiceRow(key))}
                } else {
                    cells.className = "column labels"
                    for (const key of column.labels) {
                        cells.append(key === null ? Object.assign(document.createElement("div"), {className: "gap"}) : row(key))
                    }
                }
                group.append(cells)
            }
        }

        // a lone parameter as a row in a display's corner
        const corner = (key, screen) => {
            const element = row(key)
            element.classList.add("corner")
            screen.append(element)
            return element
        }

        return {knob, choice, choiceRow, row, control, layout, corner, update, dragging}
    }

    // --- displays -----------------------------------------------------------------------------------------------
    // On the panel itself, no background: traces in the studio's display blue (DisplayPaint) with a fill fading
    // downwards, captions faint. A scale with numbers needs no lines, the numbers place it; nothing along a display's
    // edges either. Lines are references (dashed) and the grid of a display without numbers ("grid").

    const ink = alpha => `hsla(200, 83%, 60%, ${alpha})`
    // the canvas at the zoom times the screen's pixel ratio, the context in CSS pixels; answers the size
    const fit = (canvas, context, zoom = 1) => {
        const ratio = (window.devicePixelRatio || 1) * zoom
        const width = canvas.clientWidth, height = canvas.clientHeight
        const pixelWidth = Math.round(width * ratio), pixelHeight = Math.round(height * ratio)
        if (canvas.width !== pixelWidth || canvas.height !== pixelHeight) {
            canvas.width = pixelWidth
            canvas.height = pixelHeight
        }
        context.setTransform(pixelWidth / width, 0, 0, pixelHeight / height, 0, 0)
        context.clearRect(0, 0, width, height)
        return {width, height}
    }
    const gridLine = (context, x0, y0, x1, y1, kind) => {
        if (!kind) {return}
        const dashed = kind === true
        context.save()
        context.strokeStyle = dashed ? "rgba(200, 225, 240, 0.12)" : "rgba(200, 225, 240, 0.045)"
        context.lineWidth = 1
        if (dashed) {context.setLineDash([2, 2])}
        context.beginPath()
        context.moveTo(x0, y0)
        context.lineTo(x1, y1)
        context.stroke()
        context.restore()
    }
    const caption = (context, text, x, y, align = "left", alpha = 0.85) => {
        context.save()
        context.font = "400 7px Rubik, sans-serif"
        context.textAlign = align
        context.fillStyle = `hsla(197, 10%, 60%, ${alpha * 0.6})`
        context.fillText(text, x, y)
        context.restore()
    }
    const trace = (context, points, {fill = null, width = 1.25, alpha = 0.85} = {}) => {
        if (points.length < 2) {return}
        context.save()
        context.globalAlpha = alpha
        context.lineJoin = "round"
        context.lineCap = "round"
        if (fill !== null) {
            const top = Math.min(...points.map(([, y]) => y))
            context.beginPath()
            context.moveTo(points[0][0], fill)
            for (const [x, y] of points) {context.lineTo(x, y)}
            context.lineTo(points[points.length - 1][0], fill)
            context.closePath()
            const gradient = context.createLinearGradient(0, top, 0, fill)
            gradient.addColorStop(0, ink(0.16))
            gradient.addColorStop(1, ink(0))
            context.fillStyle = gradient
            context.fill()
        }
        context.beginPath()
        points.forEach(([x, y], index) => index === 0 ? context.moveTo(x, y) : context.lineTo(x, y))
        context.strokeStyle = ink(1)
        context.lineWidth = width
        context.stroke()
        context.restore()
    }
    const dot = (context, x, y, radius = 2) => {
        context.save()
        context.fillStyle = ink(0.95)
        context.beginPath()
        context.arc(x, y, Math.min(radius, 2), 0, Math.PI * 2)
        context.fill()
        context.restore()
    }

    // --- the plugin ---------------------------------------------------------------------------------------------
    // Messages to the plugin (modules/opendaw-clap/opendaw_page.h), little endian: the type at byte 0, the parameter
    // id at 4, a value (f64) at 8. Hovering reports the parameter under the pointer (clap.param-hovered); while a
    // drag runs the hover stays on its parameter.

    const PAGE = {POLL: 1, BEGIN: 2, SET: 3, END: 4, HOVER: 7}
    const NO_PARAM = 0xffffffff
    const pluginHost = (post = bytes => window.parent.postMessage(bytes, "*")) => {
        const send = (type, id, value) => {
            const view = new DataView(new ArrayBuffer(type === PAGE.SET ? 16 : 8))
            view.setUint8(0, type)
            view.setUint32(4, id, true)
            if (type === PAGE.SET) {view.setFloat64(8, value, true)}
            post(view.buffer)
        }
        let hovered = NO_PARAM, dragging = 0
        return {
            set: (id, value) => send(PAGE.SET, id, value),
            begin: id => {
                dragging++
                send(PAGE.BEGIN, id)
            },
            end: id => {
                dragging = Math.max(0, dragging - 1)
                send(PAGE.END, id)
            },
            hover: id => {
                const next = id ?? NO_PARAM
                if (next === hovered || (next === NO_PARAM && dragging > 0)) {return}
                hovered = next
                send(PAGE.HOVER, next)
            },
            poll: () => post(new Uint8Array([PAGE.POLL]).buffer)
        }
    }
    // a host that sends nothing: previews
    const quietHost = () => ({set: () => {}, begin: () => {}, end: () => {}, hover: () => {}, poll: () => {}})

    // A JUCE kit plugin (modules/juce_webclap), its device panel a page of its own beside the editor's: the kit's
    // protocol (juce_webclap_protocol.h), values normalised 0..1. Frames: u8 type, u8 flags, u16 reserved, payload.
    // hello() asks for the snapshot; set, begin, end, hover and show as the editor's page sends them; text(id, value)
    // is the plugin's own text for a value, cached, asked for in batches (onText when new texts came).
    // listen: {onSnapshot(values), onValue(id, value), onText(), onFrame(type, view)} (onFrame: streams and the port's
    // own frames).
    const KIT = {HELLO: 1, SNAPSHOT: 2, PARAM: 3, GESTURE: 4, STREAM: 6, SHOW: 9, HOVER: 10, TEXT: 11}
    const kitHost = (listen = {}, post = bytes => window.parent.postMessage(bytes, "*")) => {
        const values = new Map()
        const texts = new Map()
        const asked = new Map()
        let hovered = NO_PARAM, dragging = 0, due = false
        const frame = (type, size, write) => {
            const view = new DataView(new ArrayBuffer(4 + size))
            view.setUint8(0, type)
            write?.(view)
            post(view.buffer)
        }
        const key = (id, value) => `${id}:${value}`
        const flush = () => {
            due = false
            const pairs = [...asked.values()]
            asked.clear()
            if (pairs.length === 0) {return}
            frame(KIT.TEXT, 4 + pairs.length * 12, view => {
                view.setUint32(4, pairs.length, true)
                pairs.forEach(([id, value], index) => {
                    view.setUint32(8 + index * 12, id, true)
                    view.setFloat64(12 + index * 12, value, true)
                })
            })
        }
        const decoder = new TextDecoder()
        addEventListener("message", event => {
            if (!(event.data instanceof ArrayBuffer) || event.data.byteLength < 4) {return}
            const view = new DataView(event.data)
            const type = view.getUint8(0)
            if (type === KIT.SNAPSHOT) {
                const count = view.getUint32(4, true)
                for (let index = 0; index < count; index++) {
                    values.set(view.getUint32(8 + index * 12, true), view.getFloat64(12 + index * 12, true))
                }
                listen.onSnapshot?.(values)
            } else if (type === KIT.PARAM) {
                const id = view.getUint32(4, true), value = view.getFloat64(8, true)
                values.set(id, value)
                listen.onValue?.(id, value)
            } else if (type === KIT.TEXT) {
                const count = view.getUint32(4, true)
                let at = 8
                for (let index = 0; index < count && at + 14 <= view.byteLength; index++) {
                    const id = view.getUint32(at, true), value = view.getFloat64(at + 4, true), length = view.getUint16(at + 12, true)
                    texts.set(key(id, value), decoder.decode(new Uint8Array(event.data, at + 14, length)))
                    at += 14 + length
                }
                listen.onText?.()
            } else {
                listen.onFrame?.(type, view)
            }
        })
        return {
            values,
            hello: () => frame(KIT.HELLO, 4, view => view.setUint32(4, 1, true)),
            set: (id, value) => {
                values.set(id, value)
                frame(KIT.PARAM, 12, view => {
                    view.setUint32(4, id, true)
                    view.setFloat64(8, value, true)
                })
            },
            begin: id => {
                dragging++
                frame(KIT.GESTURE, 5, view => {
                    view.setUint32(4, id, true)
                    view.setUint8(8, 1)
                })
            },
            end: id => {
                dragging = Math.max(0, dragging - 1)
                frame(KIT.GESTURE, 5, view => view.setUint32(4, id, true))
            },
            hover: id => {
                const next = id ?? NO_PARAM
                if (next === hovered || (next === NO_PARAM && dragging > 0)) {return}
                hovered = next
                frame(KIT.HOVER, 4, view => view.setUint32(4, next, true))
            },
            show: () => frame(KIT.SHOW, 0),
            // the text if known, else asked for (with the next batch) and "" meanwhile
            text: (id, value) => {
                const known = texts.get(key(id, value))
                if (known !== undefined) {return known}
                if (!asked.has(key(id, value))) {
                    asked.set(key(id, value), [id, value])
                    if (!due) {
                        due = true
                        queueMicrotask(flush)
                    }
                }
                return ""
            }
        }
    }

    // JUCE's String::hashCode, the kit's CLAP id of a parameter (juce_webclap_bridge.h clapIdFor)
    const juceId = text => {
        let hash = 0
        for (const letter of text) {hash = (Math.imul(hash, 31) + letter.codePointAt(0)) >>> 0}
        return hash
    }

    // A JUCE kit plugin's device panel page (ports/zl-*/ui/panel.html): the card (a display, then the controls) on
    // the plugin's values, texts as the plugin writes them, a click on the display opens the plugin's window.
    //   width        the panel's width at 176 px, as the plugin declares it
    //   screen       the display's width
    //   params       {key: {param: the JUCE parameter ID, label, kind "range" | "steps", labels (steps), bipolar,
    //                unit, format}}; a choice or a switch is "steps", its JUCE value index / (count - 1); unit
    //                follows the plugin's text where the plugin writes none (" dB", "%"), format(text) rewrites it
    //                ("1 bar", "2 bars"), active(panel) false while the plugin's editor disables the control
    //   layout       the columns (controls.layout), corner a lone parameter in the display's corner
    //   edit         a button's text in the display's top left corner that opens the plugin's window ("Edit")
    //   window       false for a plugin without a window (its device panel alone): a click on the display opens
    //                nothing
    //   draw(context, width, height, panel)   the display, every frame
    //   onFrame(type, view, panel)            the port's own frames (the kit's streams and the port's types)
    //   hit(x, y, panel)      a step on the display under the pointer, {key, value} (a "steps" parameter, its value
    //                         index), or null: a click selects it as one gesture, hovering reports its parameter;
    //                         a click elsewhere opens the window
    //   opened(panel)                         after the snapshot: the port's own requests (an analyzer switched on)
    // panel: value(key) the JUCE value 0..1, plain(key) the plugin's text as a number (NaN until it came),
    // text(key), valueOf(id) and plainOf(id) the same for any parameter by its JUCE ID (one without a control: an
    // equalizer's bands), send(bytes) a frame of the port's own, time in seconds.
    const kitPanel = spec => {
        const params = new Map(Object.entries(spec.params).map(([key, p]) => [key, {kind: "range", ...p,
            id: juceId(p.param), ...(p.kind === "steps" ? {low: 0, high: p.labels.length - 1} : {})}]))
        let controls = null
        const kit = kitHost({
            onSnapshot: () => {
                controls.update()
                spec.opened?.(panel)
            },
            onValue: () => controls.update(),
            onText: () => controls.update(),
            onFrame: (type, view) => spec.onFrame?.(type, view, panel)
        })
        const norm = key => kit.values.get(params.get(key).id) ?? 0
        const panel = {
            time: 0,
            value: norm,
            text: key => kit.text(params.get(key).id, norm(key)),
            plain: key => parseFloat(kit.text(params.get(key).id, norm(key))),
            valueOf: name => kit.values.get(juceId(name)) ?? 0,
            plainOf: name => {
                const id = juceId(name)
                return parseFloat(kit.text(id, kit.values.get(id) ?? 0))
            },
            send: bytes => window.parent.postMessage(bytes, "*")
        }
        for (const p of params.values()) {if (p.active !== undefined) {p.isActive = () => p.active(panel)}}
        const card = document.createElement("div")
        card.className = "card"
        card.style.width = `${spec.width}px`
        card.innerHTML = `<div class="screen"><canvas></canvas></div><div class="group"></div>`
        const screen = card.querySelector(".screen"), canvas = card.querySelector("canvas")
        screen.style.width = `${spec.screen}px`
        if (spec.window !== false) {screen.style.cursor = "pointer"}
        const hitAt = event => {
            if (spec.hit === undefined || event.target !== canvas) {return null}
            const bounds = canvas.getBoundingClientRect()
            return spec.hit((event.clientX - bounds.left) / zoom, (event.clientY - bounds.top) / zoom, panel)
        }
        screen.addEventListener("click", event => {
            if (event.target !== canvas) {return}
            const hit = hitAt(event)
            if (hit === null) {
                if (spec.window !== false) {kit.show()}
                return
            }
            const p = params.get(hit.key)
            kit.begin(p.id)
            kit.set(p.id, hit.value / Math.max(1, p.high))
            kit.end(p.id)
            controls.update()
        })
        screen.addEventListener("pointermove", event => {
            const hit = hitAt(event)
            kit.hover(hit === null ? null : params.get(hit.key).id)
        })
        screen.addEventListener("pointerleave", () => kit.hover(null))
        controls = OpenDAWPanel.controls({
            param: key => params.get(key),
            get: key => {
                const p = params.get(key)
                return p.kind === "steps" ? Math.round(norm(key) * p.high) : norm(key)
            },
            set: (key, value) => {
                const p = params.get(key)
                kit.set(p.id, p.kind === "steps" ? value / Math.max(1, p.high) : value)
                controls.update()
            },
            text: (key, value) => {
                const p = params.get(key)
                if (p.kind === "steps") {return p.labels[value]}
                const text = kit.text(p.id, value)
                if (text === "") {return text}
                return p.format !== undefined ? p.format(text) : p.unit === undefined ? text : `${text}${p.unit}`
            },
            host: kit
        })
        controls.layout(spec.layout, card.querySelector(".group"))
        if (spec.corner !== undefined) {controls.corner(spec.corner, screen)}
        if (spec.edit !== undefined) {
            const edit = Object.assign(document.createElement("span"), {className: "edit", textContent: spec.edit})
            edit.addEventListener("click", event => {
                event.stopPropagation()
                kit.show()
            })
            screen.append(edit)
        }
        const stage = document.getElementById("stage")
        stage.append(card)
        let zoom = 1
        zoomToWindow(stage, spec.width, value => {zoom = value})
        const context = canvas.getContext("2d")
        const frame = now => {
            panel.time = now / 1000
            const {width, height} = fit(canvas, context, zoom)
            spec.draw?.(context, width, height, panel)
            requestAnimationFrame(frame)
        }
        document.fonts.ready.then(() => {
            kit.hello()
            requestAnimationFrame(frame)
        })
        return panel
    }

    // the card zoomed to the window (1:1 in the 176 px device panel); answers the zoom
    const zoomToWindow = (stage, width, onZoom) => {
        const resize = () => {
            const zoom = Math.max(0.25, Math.min(innerWidth / width, innerHeight / 176))
            stage.style.transform = `scale(${zoom})`
            onZoom(zoom)
        }
        addEventListener("resize", resize)
        resize()
    }

    return {SIZES, clamp, controls, ink, fit, gridLine, caption, trace, dot, polar, PAGE, pluginHost, quietHost, kitHost,
        juceId, kitPanel, zoomToWindow}
})()
