// openDAW device panel controls, after docs/device-panel-design.md: knobs in three sizes, choices, parameter rows, the
// layout of a card's controls, the drawing kit for displays and the bridge to the plugin. A plain script (no module),
// so a page opened from file:// works too; it defines `OpenDAWPanel`. A port copies this folder next to its page.
//
// Controls work on parameters the page describes: {id, label, kind: "range" | "steps", low, high (steps), labels
// (steps), bipolar (range)}. A range parameter's value is its knob position 0..1, a stepped one's the step. The page
// keeps the values; the controls read and write them through the context they are made with.
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
                drag = {pointerId: event.pointerId, unit: get(key)}
                element.classList.add("active")
                dragging.add(key)
                host.begin(p.id)
            })
            element.addEventListener("pointermove", event => {
                host.hover(p.id)
                if (drag === null || event.pointerId !== drag.pointerId) {return}
                drag.unit = clamp(drag.unit - event.movementY / 128 * (event.altKey || event.shiftKey ? 0.2 : 1.2), 0, 1)
                set(key, drag.unit)
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
                view.update(get(key))
                readout.textContent = text(key, get(key))
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
            const texts = p.kind === "steps"
                ? Array.from({length: p.high - p.low + 1}, (_, index) => text(key, p.low + index))
                : Array.from({length: 101}, (_, index) => text(key, index / 100))
            value.style.minWidth = `${Math.max(...texts.map(text => text.length))}ch`
            views.push(() => {value.textContent = text(key, get(key))})
            draggable(element, key, () => step(key, get(key) >= p.high ? p.low : get(key) + 1))
            return element
        }

        // a stepped parameter is a choice, any other a knob
        const control = (key, size) => param(key).kind === "steps" ? choice(key) : knob(key, size)

        // The columns of a card's controls, left to right: {large: key} the main knob alone, centred by its circle;
        // {knobs: [[key, size] | null, ...], rows} up to three per column (rows 2: a square of four); {stack: [[key,
        // size], ...]} top to bottom; {labels: [key | null, ...]} parameter rows, null a gap between groups.
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

        return {knob, choice, row, control, layout, corner, update, dragging}
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

    return {SIZES, clamp, controls, ink, fit, gridLine, caption, trace, dot, polar, PAGE, pluginHost, quietHost, zoomToWindow}
})()
