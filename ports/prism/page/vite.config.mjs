// Builds Prism's plugin UI (src/plugin-ui, copied next to this file by build.sh) with the WebCLAP backend into the
// bundle: index.html and its assets, relative to the page.
import {resolve} from "node:path"
import {defineConfig} from "vite"
import react from "@vitejs/plugin-react"

const root = import.meta.dirname

export default defineConfig({
    root: resolve(root, "src/plugin-ui"),
    base: "./",
    plugins: [react()],
    resolve: {alias: {"@": resolve(root, "src")}},
    logLevel: "warn",
    build: {
        outDir: process.env.OUT,
        emptyOutDir: false,
        assetsInlineLimit: 0,
        chunkSizeWarningLimit: 2048,
        rollupOptions: {output: {entryFileNames: "assets/prism.js", assetFileNames: "assets/[name][extname]"}}
    }
})
