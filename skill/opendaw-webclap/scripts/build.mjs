// Builds a plugin folder exactly as opendaw-commons' check does: source/build.sh with $OUT and $CLAP_INCLUDE, then
// packed into builds/<slug>.wclap.tar.gz next to the folder (the same bytes, so the same id, as the check's build).
//
//     node scripts/build.mjs ./my-plugin
import fs from "node:fs"
import path from "node:path"
import {archiveOf, buildsOf, fail, listFiles, pack, pluginFolder, requireToolchain, run, toolchainEnv} from "./lib.mjs"

const {folder, slug} = pluginFolder(process.argv[2])
const toolchain = requireToolchain()
const builds = buildsOf(folder)
const out = path.join(builds, `.out-${slug}`)
fs.rmSync(out, {recursive: true, force: true})
fs.mkdirSync(out, {recursive: true})
const started = performance.now()
const result = run("bash", [path.join(folder, "source", "build.sh")], {env: {...toolchainEnv(toolchain), OUT: out}})
if (result.status !== 0) {
    const tail = `${result.stdout}${result.stderr}`.trim().split("\n").slice(-40).join("\n")
    fail(`build.sh failed:\n${tail}`)
}
const warnings = `${result.stdout}${result.stderr}`.split("\n").filter(line => /warning:/.test(line))
const files = listFiles(out)
if (!files.some(file => file.path === "module.wasm")) {fail("build.sh wrote no module.wasm into $OUT.")}
if (!files.some(file => file.path === "index.html")) {fail("build.sh wrote no index.html (the page) into $OUT.")}
const bytes = pack(slug, files)
const archive = archiveOf(folder, slug)
// whole or not at all: the preview watches this file
fs.writeFileSync(`${archive}.part`, bytes)
fs.renameSync(`${archive}.part`, archive)
fs.rmSync(out, {recursive: true, force: true})
const kilobytes = (bytes.byteLength / 1024).toFixed(1)
if (warnings.length > 0) {console.log(warnings.slice(0, 20).join("\n"))}
console.log(`✓ Built ${path.relative(process.cwd(), archive) || archive} (${kilobytes} KB, ${files.length} files) in ${((performance.now() - started) / 1000).toFixed(1)} s`)
