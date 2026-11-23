// Shows a plugin on the open-webclap site in openDAW's engine, from this computer: serves the site on localhost with
// the build at /__local/, opens the browser, and reloads the plugin in place after every build (settings kept). The
// page reports what only a browser can tell (the panel's measured width, page errors) to builds/<slug>.report.json
// and to this output. Keep it running while working on the plugin.
//
//     node scripts/preview.mjs ./my-plugin [--port 8150] [--no-open]
import fs from "node:fs"
import http from "node:http"
import path from "node:path"
import {SITE, archiveOf, fail, pluginFolder, reportOf, run} from "./lib.mjs"

const {folder, slug} = pluginFolder(process.argv[2])
const option = name => {
    const index = process.argv.indexOf(`--${name}`)
    return index > 0 ? process.argv[index + 1] : undefined
}
const site = (option("site") ?? SITE).replace(/\/$/, "")
const archive = archiveOf(folder, slug)
const file = path.basename(archive)
const reportFile = reportOf(folder, slug)
if (!fs.existsSync(archive)) {fail(`${archive} does not exist. Build first: node scripts/build.mjs ${process.argv[2]}`)}

const isolation = {
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Embedder-Policy": "require-corp",
    "Cross-Origin-Resource-Policy": "cross-origin"
}
const cache = new Map()
const listeners = new Set()
const reports = {}

const proxy = async (request, response) => {
    const url = request.url ?? "/"
    const cached = cache.get(url)
    if (cached !== undefined) {
        response.writeHead(200, {...isolation, "Content-Type": cached.type})
        return response.end(cached.body)
    }
    let upstream
    try {
        upstream = await fetch(`${site}${url}`, {headers: {"Accept": request.headers.accept ?? "*/*"}})
    } catch (error) {
        response.writeHead(502, {"Content-Type": "text/plain"})
        return response.end(`Cannot reach ${site}: ${error}. Is the computer online?`)
    }
    const type = upstream.headers.get("content-type") ?? "application/octet-stream"
    const body = Buffer.from(await upstream.arrayBuffer())
    // the site is a single page app: GitHub Pages answers its routes (/preview) with the app and a 404
    const status = upstream.status === 404 && type.startsWith("text/html") ? 200 : upstream.status
    if (status === 200 && /\/assets\/|\.(wasm|js|css|svg|png|woff2?)$/.test(url.split("?")[0])) {cache.set(url, {type, body})}
    response.writeHead(status, {...isolation, "Content-Type": type, "Cache-Control": "no-cache"})
    response.end(body)
}

const printReport = facts => {
    if (facts.error !== undefined) {
        console.log(`✗ page error (${facts.panel ? "panel" : "window"}): ${facts.error}`)
        return
    }
    const {declaredWidth, contentWidth} = facts
    if (declaredWidth === undefined || contentWidth === undefined) {return}
    if (contentWidth > declaredWidth) {
        console.log(`✗ the panel's content is ${contentWidth} px wide, the plugin declares ${declaredWidth} px: set panelWidth to ${contentWidth} in INFO and build again`)
    } else if (declaredWidth - contentWidth > 6) {
        console.log(`! the panel has ${declaredWidth - contentWidth} px to spare: set panelWidth to ${contentWidth} in INFO, devices stay as narrow as their controls`)
    } else {
        console.log(`✓ the panel's content fits its ${declaredWidth} px`)
    }
}

const server = http.createServer(async (request, response) => {
    const url = request.url ?? "/"
    if (url === `/__local/${encodeURIComponent(file)}` || url === `/__local/${file}`) {
        if (!fs.existsSync(archive)) {
            response.writeHead(404, {...isolation, "Content-Type": "text/plain"})
            return response.end("not built")
        }
        response.writeHead(200, {...isolation, "Content-Type": "application/gzip", "Cache-Control": "no-store"})
        return response.end(fs.readFileSync(archive))
    }
    if (url === "/__local/events") {
        response.writeHead(200, {...isolation, "Content-Type": "text/event-stream", "Cache-Control": "no-store", "Connection": "keep-alive"})
        response.write(": connected\n\n")
        listeners.add(response)
        request.on("close", () => listeners.delete(response))
        return
    }
    if (url === "/__local/report" && request.method === "POST") {
        let body = ""
        request.on("data", chunk => body += chunk)
        request.on("end", () => {
            try {
                const facts = JSON.parse(body)
                const key = facts.error !== undefined ? "errors" : facts.panel ? "panel" : "window"
                if (key === "errors") {(reports.errors ??= []).push(facts.error)} else {reports[key] = facts}
                fs.writeFileSync(reportFile, JSON.stringify({...reports, time: new Date().toISOString()}, null, 2))
                if (key !== "window") {printReport(facts)}
            } catch (_) {}
            response.writeHead(204, isolation)
            response.end()
        })
        return
    }
    proxy(request, response).catch(error => {
        response.writeHead(500, {"Content-Type": "text/plain"})
        response.end(String(error))
    })
})

// a rebuild: the page loads the new archive and keeps the settings
let lastChange = fs.statSync(archive).mtimeMs
fs.watchFile(archive, {interval: 400}, stats => {
    if (stats.mtimeMs === 0 || stats.mtimeMs === lastChange) {return}
    lastChange = stats.mtimeMs
    delete reports.panel
    delete reports.window
    delete reports.errors
    console.log(`↻ ${file} rebuilt, the preview reloads it`)
    for (const listener of listeners) {listener.write("event: change\ndata: 1\n\n")}
})

const listen = port => new Promise((resolve, reject) => {
    server.once("error", reject)
    server.listen(port, "127.0.0.1", () => resolve(port))
})
const first = Number(option("port") ?? 8150)
let port = null
for (let candidate = first; candidate < first + 20 && port === null; candidate++) {
    port = await listen(candidate).catch(() => null)
}
if (port === null) {fail(`No free port from ${first} on`)}
const address = `http://localhost:${port}/preview?bundle=${encodeURIComponent(file)}`
fs.rmSync(reportFile, {force: true})
console.log(`✓ Preview of ${slug}: ${address}`)
console.log(`  Click "Start audio" on the page. ${slug} reloads after every build. Reports: ${reportFile}`)
if (!process.argv.includes("--no-open")) {
    const opener = process.platform === "darwin" ? "open" : process.platform === "win32" ? "explorer" : "xdg-open"
    run(opener, [address])
}
