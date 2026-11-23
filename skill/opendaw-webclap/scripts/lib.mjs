// Shared by the skill's scripts: where the toolchain lives, the plugin folder, packing a bundle the way
// opendaw-commons' check packs it (so the local archive has the same bytes and id), reading one back.
import fs from "node:fs"
import os from "node:os"
import path from "node:path"
import zlib from "node:zlib"
import {spawnSync} from "node:child_process"
import {fileURLToPath} from "node:url"

export const SKILL_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..")
export const HOME = process.env.OPENDAW_WEBCLAP_HOME ?? path.join(os.homedir(), ".opendaw-webclap")
// the versions opendaw-commons' check builds with (.github/workflows/validate.yml there)
export const EMSDK_VERSION = "3.1.51"
export const CLAP_COMMIT = "a47f6badb49d948fd009998f28309cdab78979c9"
export const COMMONS_REPO = "andremichelle/opendaw-commons"
export const CLAP_ID_PREFIX = "studio.opendaw.commons."
export const SLUG = /^[a-z0-9]+(?:-[a-z0-9]+)*$/
export const SITE = "https://webclap.opendaw.studio"

export const fail = message => {
    console.error(`\n✗ ${message}`)
    process.exit(1)
}

export const run = (command, args, options = {}) => spawnSync(command, args, {encoding: "utf8", maxBuffer: 256 * 1024 * 1024, ...options})

export const has = command => run(process.platform === "win32" ? "where" : "which", [command]).status === 0

// --- the toolchain ------------------------------------------------------------------------------------------------

export const toolchainFile = path.join(HOME, "toolchain.json")

export const readToolchain = () => {
    if (!fs.existsSync(toolchainFile)) {return null}
    return JSON.parse(fs.readFileSync(toolchainFile, "utf8"))
}

// without another Emscripten's settings (an older emsdk's EMSDK_PYTHON breaks ours)
export const cleanEnv = () => Object.fromEntries(Object.entries(process.env)
    .filter(([key]) => !["EMSDK", "EMSDK_PYTHON", "EMSDK_NODE", "EM_CONFIG", "EM_CACHE"].includes(key)))

// the environment build.sh runs in: emcc on the path, the CLAP headers
export const toolchainEnv = toolchain => {
    const env = {...(toolchain.emsdk !== null ? cleanEnv() : process.env), CLAP_INCLUDE: toolchain.clapInclude}
    if (toolchain.emsdk !== null) {
        env.EMSDK = toolchain.emsdk
        env.EM_CONFIG = path.join(toolchain.emsdk, ".emscripten")
        env.PATH = [path.join(toolchain.emsdk, "upstream", "emscripten"), toolchain.emsdk, process.env.PATH].join(path.delimiter)
    }
    return env
}

export const emccVersion = env => {
    const result = run("emcc", ["--version"], {env})
    if (result.status !== 0) {return null}
    return /(\d+\.\d+\.\d+)/.exec(result.stdout)?.[1] ?? null
}

export const requireToolchain = () => {
    const toolchain = readToolchain()
    if (toolchain === null) {fail("The toolchain is not installed yet. Run: node scripts/setup.mjs")}
    return toolchain
}

// --- the plugin folder: <slug>/meta.json, <slug>/source/build.sh ----------------------------------------------------

export const pluginFolder = argument => {
    if (argument === undefined) {fail("Name the plugin folder, for example: node scripts/build.mjs ./my-plugin")}
    const folder = path.resolve(argument)
    if (!fs.existsSync(path.join(folder, "meta.json"))) {fail(`${folder} has no meta.json: is it a plugin folder?`)}
    if (!fs.existsSync(path.join(folder, "source", "build.sh"))) {fail(`${folder}/source has no build.sh`)}
    const slug = path.basename(folder)
    if (!SLUG.test(slug) || slug.length > 64) {fail(`The folder name "${slug}" must be lowercase letters, digits and single hyphens.`)}
    return {folder, slug, meta: JSON.parse(fs.readFileSync(path.join(folder, "meta.json"), "utf8"))}
}

// builds live next to the plugin folder, never inside it (opendaw-commons takes source only)
export const buildsOf = folder => path.join(path.dirname(folder), "builds")
export const archiveOf = (folder, slug) => path.join(buildsOf(folder), `${slug}.wclap.tar.gz`)
export const reportOf = (folder, slug) => path.join(buildsOf(folder), `${slug}.report.json`)

// --- packing, byte for byte as opendaw-commons (tools/src/Wclaps.ts) ------------------------------------------------

export const listFiles = (directory, prefix = "") =>
    fs.readdirSync(directory, {withFileTypes: true})
        .filter(entry => !entry.name.startsWith("."))
        .toSorted((left, right) => left.name < right.name ? -1 : left.name > right.name ? 1 : 0)
        .flatMap(entry => {
            const absolute = path.join(directory, entry.name)
            const relative = `${prefix}${entry.name}`
            return entry.isDirectory()
                ? listFiles(absolute, `${relative}/`)
                : [{path: relative, bytes: new Uint8Array(fs.readFileSync(absolute))}]
        })

const octal = (value, width) => value.toString(8).padStart(width - 1, "0") + "\0"

export const tar = (root, files) => {
    const blocks = []
    for (const file of files) {
        const header = new Uint8Array(512)
        const write = (text, offset) => header.set(new TextEncoder().encode(text), offset)
        const name = `${root}/${file.path}`
        if (new TextEncoder().encode(name).length > 100) {fail(`${file.path}: path longer than 100 bytes`)}
        write(name, 0)
        write(octal(0o644, 8), 100)
        write(octal(0, 8), 108)
        write(octal(0, 8), 116)
        write(octal(file.bytes.length, 12), 124)
        write(octal(0, 12), 136)
        write("        ", 148)
        write("0", 156)
        write("ustar\u000000", 257)
        const checksum = header.reduce((sum, byte) => sum + byte, 0)
        write(octal(checksum, 7) + " ", 148)
        blocks.push(header, file.bytes, new Uint8Array((512 - file.bytes.length % 512) % 512))
    }
    blocks.push(new Uint8Array(1024))
    return Buffer.concat(blocks)
}

export const pack = (slug, files) => files.length === 1
    ? files[0].bytes
    : new Uint8Array(zlib.gzipSync(tar(`${slug}.wclap`, files), {level: 9}))

// the files of a packed bundle (.wclap.tar.gz), without the top directory
export const unpack = bytes => {
    const data = bytes[0] === 0x1f && bytes[1] === 0x8b ? zlib.gunzipSync(bytes) : null
    if (data === null) {return [{path: "module.wasm", bytes}]}
    const files = []
    let offset = 0
    while (offset + 512 <= data.length) {
        const header = data.subarray(offset, offset + 512)
        if (header.every(byte => byte === 0)) {break}
        const text = (start, length) => new TextDecoder().decode(header.subarray(start, start + length)).replace(/\0.*$/s, "")
        const prefix = text(345, 155)
        const name = prefix.length > 0 ? `${prefix}/${text(0, 100)}` : text(0, 100)
        const size = parseInt(text(124, 12).trim() || "0", 8)
        const type = text(156, 1)
        offset += 512
        if (type === "0" || type === "") {
            files.push({path: name.split("/").slice(1).join("/"), bytes: data.slice(offset, offset + size)})
        }
        offset += Math.ceil(size / 512) * 512
    }
    return files
}
