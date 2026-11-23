// The opendaw-webclap agent skill as it is published: skill/opendaw-webclap plus the shared kit files from modules/
// and the guides from docs/, so the published skill never drifts from the repository. The site serves the result
// under /.well-known/agent-skills/ (vite.config.ts), which is what `npx skills add https://webclap.opendaw.studio`
// reads: index.json lists every skill and its files.
import fs from "node:fs"
import path from "node:path"
import {fileURLToPath} from "node:url"

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..")
const skillRoot = path.join(root, "skill", "opendaw-webclap")

const walk = (directory, prefix = "") => fs.readdirSync(directory, {withFileTypes: true})
    .filter(entry => !entry.name.startsWith("."))
    .toSorted((left, right) => left.name < right.name ? -1 : left.name > right.name ? 1 : 0)
    .flatMap(entry => entry.isDirectory()
        ? walk(path.join(directory, entry.name), `${prefix}${entry.name}/`)
        : [{path: `${prefix}${entry.name}`, source: path.join(directory, entry.name)}])

export const SKILL_NAME = "opendaw-webclap"

// [{path, source}]: the published path inside the skill and the file it comes from
export const skillFiles = () => [
    ...walk(skillRoot),
    {path: "kit/opendaw_page.h", source: path.join(root, "modules", "opendaw-clap", "opendaw_page.h")},
    {path: "kit/panel.js", source: path.join(root, "modules", "opendaw-page", "panel.js")},
    {path: "kit/panel.css", source: path.join(root, "modules", "opendaw-page", "panel.css")},
    {path: "references/building-for-opendaw.md", source: path.join(root, "docs", "building-for-opendaw.md")},
    {path: "references/device-panel-design.md", source: path.join(root, "docs", "device-panel-design.md")}
]

const frontmatter = text => {
    const block = /^---\n([\s\S]*?)\n---/.exec(text)?.[1] ?? ""
    return Object.fromEntries(block.split("\n").map(line => /^(\w+):\s*(.*)$/.exec(line)).filter(Boolean)
        .map(([, key, value]) => [key, value.trim()]))
}

// the discovery index (agent skills well-known format, version 0.1: name, description, files)
export const skillIndex = files => {
    const {name, description} = frontmatter(fs.readFileSync(path.join(skillRoot, "SKILL.md"), "utf8"))
    if (name !== SKILL_NAME || description === undefined) {throw new Error("SKILL.md needs name and description")}
    return {skills: [{name, description, files: files.map(file => file.path)}]}
}

// writes the published skill into a folder (the site's build, or a local test of the install)
export const writeSkill = target => {
    const files = skillFiles()
    for (const file of files) {
        const destination = path.join(target, SKILL_NAME, file.path)
        fs.mkdirSync(path.dirname(destination), {recursive: true})
        fs.copyFileSync(file.source, destination)
    }
    fs.writeFileSync(path.join(target, "index.json"), JSON.stringify(skillIndex(files), null, 2))
    return files.length
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
    const target = path.resolve(process.argv[2] ?? path.join(root, "build", "agent-skills"))
    console.log(`${writeSkill(target)} files in ${target}`)
}
