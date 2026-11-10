// Screenshots of bundles imported into the site: headless Chrome loads <base>/, hands each bundle to the header's
// Import file input, waits until the plugin is ready and clips its window at a device scale factor of 2.
// Usage: node import.mjs <base url> <out dir> <name=bundle path>...
import {spawn} from "node:child_process"
import {mkdirSync, writeFileSync, mkdtempSync} from "node:fs"
import {tmpdir} from "node:os"
import {join} from "node:path"

const [base, out, ...ids] = process.argv.slice(2)
mkdirSync(out, {recursive: true})
const CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
const port = 9335
const chrome = spawn(CHROME, ["--headless=new", `--remote-debugging-port=${port}`, "--hide-scrollbars",
    "--autoplay-policy=no-user-gesture-required", "--enable-unsafe-swiftshader",
    `--user-data-dir=${mkdtempSync(join(tmpdir(), "webclap-shots-"))}`, "--window-size=2200,1600", "about:blank"],
    {stdio: "ignore"})
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms))

let target = null
for (let i = 0; i < 50 && target === null; i++) {
    await sleep(200)
    try {
        const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json()
        target = list.find(t => t.type === "page") ?? null
    } catch {}
}
const socket = new WebSocket(target.webSocketDebuggerUrl)
await new Promise(resolve => socket.addEventListener("open", resolve))
let id = 0
const pending = new Map()
socket.addEventListener("message", event => {
    const message = JSON.parse(event.data)
    if (pending.has(message.id)) {
        pending.get(message.id)(message)
        pending.delete(message.id)
    }
})
const send = (method, params = {}) => new Promise(resolve => {
    pending.set(++id, resolve)
    socket.send(JSON.stringify({id, method, params}))
})
const evaluate = async expression => (await send("Runtime.evaluate", {expression, returnByValue: true, awaitPromise: true})).result?.result?.value

await send("Emulation.setDeviceMetricsOverride", {width: 2200, height: 1600, deviceScaleFactor: 2, mobile: false})
await send("Page.enable")
await send("DOM.enable")
for (const pair of ids) {
    const [plugin, bundle] = pair.split("=")
    await send("Page.navigate", {url: `${base}/`})
    await sleep(2500)
    const {root} = (await send("DOM.getDocument", {depth: -1})).result
    const {nodeId} = (await send("DOM.querySelector", {nodeId: root.nodeId, selector: "header input[type=file]"})).result
    await send("DOM.setFileInputFiles", {nodeId, files: [bundle]})
    // ready: the sidebar's plugin state says so (or a failure, which is reported)
    let state = ""
    for (let i = 0; i < 120; i++) {
        await sleep(500)
        state = await evaluate(`(document.body.innerText.match(/\\n(ready|failed[^\\n]*|error[^\\n]*)\\n/i) ?? [""])[0].trim()`) ?? ""
        if (state !== "") {break}
    }
    await sleep(3000) // the page paints its first frames, fonts settle
    const rect = await evaluate(`(() => {
        const frames = [...document.querySelectorAll("iframe")].filter(f => f.getBoundingClientRect().width > 0)
        // the window where there is one (the larger frame), else the device panel
        const area = f => f.getBoundingClientRect().width * f.getBoundingClientRect().height
        const frame = frames.sort((a, b) => area(b) - area(a))[0]
        if (!frame) {return null}
        const r = frame.getBoundingClientRect()
        return {kind: frames.length > 1 ? "window" : "only frame", x: r.x + scrollX, y: r.y + scrollY, width: r.width, height: r.height}
    })()`)
    if (!rect) {
        console.log(`${plugin}: no frame (${state || "not ready"})`)
        continue
    }
    const shot = await send("Page.captureScreenshot", {format: "png", captureBeyondViewport: true,
        clip: {x: rect.x, y: rect.y, width: rect.width, height: rect.height, scale: 1}})
    writeFileSync(join(out, `${plugin.split("?")[0]}.png`), Buffer.from(shot.result.data, "base64"))
    console.log(`${plugin}: ${rect.kind} ${Math.round(rect.width)} x ${Math.round(rect.height)} (${state || "not ready"})`)
}
socket.close()
chrome.kill()
