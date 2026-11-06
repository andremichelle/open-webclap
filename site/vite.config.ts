import {cpSync, existsSync, readdirSync, readFileSync} from "fs"
import {createRequire} from "module"
import {dirname, resolve} from "path"
import {defineConfig, Plugin} from "vite"
import {Plugins} from "./src/plugins"

const root = resolve(__dirname, "..")

const IsolationHeaders = {
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Embedder-Policy": "require-corp",
    "Cross-Origin-Resource-Policy": "cross-origin"
}

// The engine binaries ship inside the npm package: served under /wasm-engine/ in dev, copied into the build
const wasmSourceDir = dirname(createRequire(import.meta.url).resolve("@opendaw/studio-core-wasm/wasm-processor.js"))
const isWasmEngineAsset = (name: string): boolean =>
    name.startsWith("wasm/") && name.endsWith(".wasm") && !name.includes("..")

const wasmEngine = (): Plugin => ({
    name: "wasm-engine-assets",
    configureServer(server) {
        server.middlewares.use("/wasm-engine", (request, response, next) => {
            const name = (request.url ?? "").split("?")[0].replace(/^\//, "")
            const file = resolve(wasmSourceDir, name)
            if (!isWasmEngineAsset(name) || !existsSync(file)) {return next()}
            response.setHeader("Content-Type", "application/wasm")
            response.setHeader("Cache-Control", "no-store")
            response.end(readFileSync(file))
        })
    },
    generateBundle() {
        if (!existsSync(resolve(wasmSourceDir, "wasm"))) {
            this.error("wasm-engine-assets: no engine binaries in @opendaw/studio-core-wasm")
        }
        const walk = (relative: string): ReadonlyArray<string> =>
            readdirSync(resolve(wasmSourceDir, relative), {withFileTypes: true}).flatMap(entry =>
                entry.isDirectory() ? walk(`${relative}/${entry.name}`) : [`${relative}/${entry.name}`])
        walk("wasm").filter(isWasmEngineAsset).forEach(name => this.emitFile({
            type: "asset",
            fileName: `wasm-engine/${name}`,
            source: readFileSync(resolve(wasmSourceDir, name))
        }))
    }
})

// Dev only: the archives the build scripts leave in build/, at /bundles/<id>.wclap.tar.gz (a page opened with
// ?local loads them instead of the hosted ones, to try a build before it is uploaded)
const localBundles = (): Plugin => ({
    name: "local-bundles",
    configureServer(server) {
        server.middlewares.use("/bundles", (request, response, next) => {
            const name = (request.url ?? "").split("?")[0].replace(/^\//, "")
            const plugin = Plugins.find(plugin => name === `${plugin.id}.wclap.tar.gz`)
            if (plugin === undefined) {return next()}
            const file = resolve(root, plugin.archive)
            if (!existsSync(file)) {
                response.statusCode = 404
                return response.end(`${plugin.archive} not built (${plugin.build})`)
            }
            response.setHeader("Content-Type", "application/gzip")
            response.setHeader("Cache-Control", "no-store")
            response.end(readFileSync(file))
        })
    }
})

// GitHub Pages has no rewrites: unknown paths (a plugin page opened directly) get 404.html, the app
const spaFallback = (): Plugin => ({
    name: "spa-fallback",
    writeBundle(options) {
        cpSync(resolve(options.dir!, "index.html"), resolve(options.dir!, "404.html"))
    }
})

export default defineConfig({
    resolve: {alias: {"@": resolve(__dirname, "./src")}},
    // "/" at the domain root (upstream, webclap.opendaw.studio); a fork's GitHub Pages builds with SITE_BASE=/<repo>/
    base: process.env.SITE_BASE ?? "/",
    build: {target: "esnext", sourcemap: true},
    esbuild: {target: "esnext"},
    clearScreen: false,
    server: {
        port: 8130,
        headers: IsolationHeaders,
        // the asset host answers CORS for the site's own origin only, so dev fetches the hosted bundles through here
        proxy: {"/wclaps": {target: "https://assets.opendaw.studio", changeOrigin: true}}
    },
    preview: {port: 8130, headers: IsolationHeaders},
    plugins: [wasmEngine(), localBundles(), spaFallback()]
})
