// Starts a plugin from a template: a folder <slug> laid out as opendaw-commons wants it (meta.json, source/), with
// the kit copied into source/ (the check provides only Emscripten and the CLAP headers).
//
//     node scripts/new.mjs <audio-effect|instrument|midi-effect> <slug> --name "Tape Drive" --author "Ada" \
//         --description "Warm tape saturation." [--dir .]
import fs from "node:fs"
import path from "node:path"
import {CLAP_ID_PREFIX, SKILL_ROOT, SLUG, fail} from "./lib.mjs"

const KINDS = ["audio-effect", "instrument", "midi-effect"]
// the template's device panel at 176 px, as its page measures it
const PANEL_WIDTHS = {"audio-effect": 196, "instrument": 249, "midi-effect": 104}

const [kind, slug] = process.argv.slice(2)
const option = name => {
    const index = process.argv.indexOf(`--${name}`)
    return index > 0 ? process.argv[index + 1] : undefined
}
if (!KINDS.includes(kind)) {fail(`The first argument is the kind: ${KINDS.join(", ")}`)}
if (slug === undefined || !SLUG.test(slug) || slug.length > 64) {
    fail("The second argument is the folder name: lowercase letters, digits and single hyphens, for example tape-drive")
}
const name = option("name") ?? slug.split("-").map(word => word[0].toUpperCase() + word.slice(1)).join(" ")
const author = option("author")
const description = option("description") ?? `${name}, made with the openDAW WebCLAP kit.`
// lowercase words joined by hyphens, at most 8: what people search for
const DEFAULT_TAGS = {"audio-effect": ["effect"], "instrument": ["synth"], "midi-effect": ["midi"]}
const tags = (option("tags") ?? DEFAULT_TAGS[kind].join(",")).split(",").map(tag => tag.trim().toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, ""))
    .filter((tag, index, all) => tag.length > 0 && tag.length <= 32 && all.indexOf(tag) === index).slice(0, 8)
if (author === undefined || author.trim().length === 0) {fail("--author is needed: the name shown as the plugin's vendor")}
if (name.length > 64) {fail("--name has at most 64 characters")}
if (author.length > 64) {fail("--author has at most 64 characters")}
if (description.length > 1000) {fail("--description has at most 1000 characters")}
const target = path.resolve(option("dir") ?? ".", slug)
if (fs.existsSync(target)) {fail(`${target} exists already`)}

// the kit: assembled into kit/ when the skill is published, in the repository the modules beside it
const kitFile = (name, fallback) => {
    const assembled = path.join(SKILL_ROOT, "kit", name)
    if (fs.existsSync(assembled)) {return assembled}
    const repository = path.resolve(SKILL_ROOT, "..", "..", fallback)
    if (fs.existsSync(repository)) {return repository}
    return fail(`The kit file ${name} is missing in ${path.join(SKILL_ROOT, "kit")}`)
}

const cString = text => text.replace(/\\/g, "\\\\").replace(/"/g, "\\\"")
const html = text => text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
const json = text => JSON.stringify(text).slice(1, -1)
const replacements = file => {
    // C++ strings and comments take one line
    const quote = file.endsWith(".cpp") ? text => cString(text.replace(/\s+/g, " "))
        : file.endsWith(".json") ? json : file.endsWith(".html") ? html : text => text
    return [
        ["{{NAME}}", quote(name)], ["{{DESCRIPTION}}", quote(description)], ["{{VENDOR}}", quote(author)],
        ["{{AUTHOR}}", quote(author)], ["\"{{TAGS}}\"", tags.map(tag => JSON.stringify(tag)).join(", ")], ["{{CLAP_ID}}", `${CLAP_ID_PREFIX}${slug}`], ["{{PANEL_WIDTH}}", String(PANEL_WIDTHS[kind])]
    ]
}

const copyTemplate = (from, to) => {
    fs.mkdirSync(to, {recursive: true})
    for (const entry of fs.readdirSync(from, {withFileTypes: true})) {
        const source = path.join(from, entry.name), destination = path.join(to, entry.name)
        if (entry.isDirectory()) {
            copyTemplate(source, destination)
            continue
        }
        let text = fs.readFileSync(source, "utf8")
        for (const [key, value] of replacements(entry.name)) {text = text.split(key).join(value)}
        fs.writeFileSync(destination, text)
        fs.chmodSync(destination, fs.statSync(source).mode)
    }
}

copyTemplate(path.join(SKILL_ROOT, "templates", kind), target)
const source = path.join(target, "source")
fs.mkdirSync(path.join(source, "kit"), {recursive: true})
fs.mkdirSync(path.join(source, "page", "opendaw-page"), {recursive: true})
fs.copyFileSync(kitFile("opendaw_kit.h", "skill/opendaw-webclap/kit/opendaw_kit.h"), path.join(source, "kit", "opendaw_kit.h"))
fs.copyFileSync(kitFile("opendaw_page.h", "modules/opendaw-clap/opendaw_page.h"), path.join(source, "kit", "opendaw_page.h"))
fs.copyFileSync(kitFile("opendaw-kit.js", "skill/opendaw-webclap/kit/opendaw-kit.js"), path.join(source, "page", "opendaw-kit.js"))
fs.copyFileSync(kitFile("panel.js", "modules/opendaw-page/panel.js"), path.join(source, "page", "opendaw-page", "panel.js"))
fs.copyFileSync(kitFile("panel.css", "modules/opendaw-page/panel.css"), path.join(source, "page", "opendaw-page", "panel.css"))
fs.writeFileSync(path.join(source, "LICENSE"), `MIT License

Copyright (c) ${new Date().getFullYear()} ${author}

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
`)
console.log(`✓ Created ${path.relative(process.cwd(), target) || target} (${kind}, clap id ${CLAP_ID_PREFIX}${slug})`)
console.log("  Edit source/plugin.cpp (parameters and sound) and, for a display, source/page/index.html.")
