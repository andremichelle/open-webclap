// Extracts the web presets of the pinned faust-elements checkout (web/controls.js) as JSON for gen.py:
// [{id, tag, title, description, values}] — the [name, description] in English, and the first comment line
// above each preset as its tag (it names the exciter and resonator, e.g. "Strike, modal: ...").
//
// Usage: node extract-presets.mjs <external/faust-elements dir> <output presets.json>

import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

const [, , src, out] = process.argv;
const controls = readFileSync(join(src, "web/controls.js"), "utf8");
const { PRESETS } = await import(pathToFileURL(join(src, "web/controls.js")).href);

// id -> the comment line above the preset's block (first // line of the comment block)
const tags = {};
for (const match of controls.matchAll(/\{\s*\n\s*((?:\/\/[^\n]*\n\s*)+)id: "([^"]+)"/g)) {
    const first = match[1].split("\n").find((line) => line.includes("//"));
    if (first) tags[match[2]] = first.replace(/^\s*\/\/\s?/, "").trim();
}

const result = PRESETS.map((p) => ({
    id: p.id,
    tag: tags[p.id] ?? "",
    title: p.en[0],
    description: p.en[1],
    values: p.values,
}));

writeFileSync(out, JSON.stringify(result, null, 1) + "\n");
console.log(`extract-presets: ${result.length} presets -> ${out}`);
