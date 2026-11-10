// Screenshots of every OpenRB panel from the preview page: headless Chrome over the DevTools protocol, each .plate
// clipped at its own size, at a device scale factor of 2. Usage: node shoot.mjs <preview url> <out dir>
import {spawn} from "node:child_process"
import {mkdirSync, writeFileSync, mkdtempSync} from "node:fs"
import {tmpdir} from "node:os"
import {join} from "node:path"

const [url, out] = process.argv.slice(2)
mkdirSync(out, {recursive: true})
const CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
const port = 9333
const chrome = spawn(CHROME, ["--headless=new", `--remote-debugging-port=${port}`, "--hide-scrollbars",
    `--user-data-dir=${mkdtempSync(join(tmpdir(), "openrb-shots-"))}`, "--window-size=1600,1200", "about:blank"],
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

await send("Emulation.setDeviceMetricsOverride", {width: 1600, height: 1200, deviceScaleFactor: 2, mobile: false})
await send("Page.enable")
await send("Page.navigate", {url})
await sleep(2500) // fonts, then the made-up signals running a while
const {result} = await send("Runtime.evaluate", {
    returnByValue: true,
    expression: `[...document.querySelectorAll(".device")].map(device => {
        const plate = device.querySelector(".plate"), r = plate.getBoundingClientRect()
        return {title: device.querySelector(".caption").textContent, x: r.x + scrollX, y: r.y + scrollY, width: r.width, height: r.height}
    })`
})
const NAMES = {"OpenRB 303": "openrb-bass", "OpenRB Distortion": "openrb-distortion", "OpenRB PCF": "openrb-pcf",
    "OpenRB Compressor": "openrb-compressor", "OpenRB Delay": "openrb-delay", "OpenRB 808": "openrb-808", "OpenRB 909": "openrb-909"}
for (const plate of result.result.value) {
    const title = plate.title.split(" · ")[0]
    const shot = await send("Page.captureScreenshot", {format: "png", captureBeyondViewport: true,
        clip: {x: plate.x, y: plate.y, width: plate.width, height: plate.height, scale: 1}})
    const file = join(out, `${NAMES[title]}.png`)
    writeFileSync(file, Buffer.from(shot.result.data, "base64"))
    console.log(`${file} ${Math.round(plate.width)} x ${Math.round(plate.height)}`)
}
socket.close()
chrome.kill()
