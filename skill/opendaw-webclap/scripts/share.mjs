// Shares a plugin with every openDAW user: a pull request to opendaw-commons (wclaps/<slug>) from the user's own
// GitHub account, after the same checks the repository runs. A maintainer reviews it, after the merge it is in every
// openDAW Studio (Load Plugin > Community).
//
//     node scripts/share.mjs ./my-plugin            checks and says what it would do, changes nothing
//     node scripts/share.mjs ./my-plugin --yes      forks, pushes and opens (or updates) the pull request
//     node scripts/share.mjs ./my-plugin --status   the pull request's checks and the check's comment
import fs from "node:fs"
import path from "node:path"
import {fileURLToPath} from "node:url"
import {COMMONS_REPO, SLUG, buildsOf, fail, has, pluginFolder, run} from "./lib.mjs"

const {folder, slug, meta} = pluginFolder(process.argv[2])
const confirmed = process.argv.includes("--yes")
const statusOnly = process.argv.includes("--status")
const branch = `wclap-${slug}`
const scripts = path.dirname(fileURLToPath(import.meta.url))

const gh = (args, options = {}) => run("gh", args, options)
const requireGitHub = () => {
    if (!has("gh")) {
        fail(process.platform === "darwin" && has("brew")
            ? "The GitHub command line tool is missing. Install it: brew install gh"
            : "The GitHub command line tool is missing. Install it from https://cli.github.com")
    }
    if (gh(["auth", "status"]).status !== 0) {
        fail("Not signed in to GitHub. Run: gh auth login --web (it opens the browser; a free account is enough: https://github.com/signup)")
    }
    const login = gh(["api", "user", "--jq", ".login"]).stdout.trim()
    if (login.length === 0) {fail("Could not read the GitHub account. Run: gh auth status")}
    return login
}

const findPullRequest = login => {
    const result = gh(["pr", "list", "--repo", COMMONS_REPO, "--head", branch, "--author", login, "--state", "open",
        "--json", "url,number", "--jq", ".[0] | select(. != null) | .url"])
    return result.stdout.trim()
}

if (statusOnly) {
    const login = requireGitHub()
    const url = findPullRequest(login)
    if (url.length === 0) {fail(`No open pull request for ${slug}. Share it first: node scripts/share.mjs ${process.argv[2]} --yes`)}
    const view = JSON.parse(gh(["pr", "view", url, "--json", "state,statusCheckRollup,comments,reviewDecision"]).stdout)
    console.log(`Pull request: ${url} (${view.state.toLowerCase()}${view.reviewDecision ? `, ${view.reviewDecision.toLowerCase()}` : ""})`)
    for (const check of view.statusCheckRollup ?? []) {
        console.log(`  ${check.name ?? check.context}: ${(check.conclusion || check.status || check.state || "").toLowerCase()}`)
    }
    const report = (view.comments ?? []).filter(comment => comment.body.includes("opendaw-commons-check")).at(-1)
    if (report !== undefined) {console.log(`\nThe check says:\n${report.body.replace(/<!--.*?-->/g, "").trim()}`)}
    const others = (view.comments ?? []).filter(comment => !comment.body.includes("opendaw-commons-check"))
    for (const comment of others.slice(-5)) {console.log(`\n${comment.author?.login ?? "someone"} wrote:\n${comment.body.trim()}`)}
    process.exit(0)
}

// --- the same rules as opendaw-commons (CONTRIBUTING.md, schemas/*.meta.json) -------------------------------------

const problems = []
const allowedMeta = new Set(["name", "description", "author", "tags", "license", "credits"])
const text = (value, max) => typeof value === "string" && value.length >= 1 && value.length <= max
if (!text(meta.name, 64)) {problems.push("meta.json: name, 1 to 64 characters")}
if (!text(meta.description, 1000)) {problems.push("meta.json: description, 1 to 1000 characters")}
if (typeof meta.author !== "object" || meta.author === null || !text(meta.author.name, 64)) {problems.push("meta.json: author.name, 1 to 64 characters")}
if (meta.author?.github !== undefined && !/^[A-Za-z0-9](?:[A-Za-z0-9-]{0,38})$/.test(meta.author.github)) {problems.push("meta.json: author.github is no GitHub name")}
if (meta.author?.url !== undefined && !/^https:\/\//.test(meta.author.url)) {problems.push("meta.json: author.url starts with https://")}
for (const key of Object.keys(meta.author ?? {})) {if (!["name", "github", "url"].includes(key)) {problems.push(`meta.json: author.${key} is not allowed`)}}
if (!Array.isArray(meta.tags) || meta.tags.length > 8 || new Set(meta.tags).size !== meta.tags.length ||
    meta.tags.some(tag => typeof tag !== "string" || tag.length > 32 || !SLUG.test(tag))) {
    problems.push("meta.json: tags, up to 8 different lowercase words joined by hyphens")
}
if (meta.tags?.length === 0) {problems.push("meta.json: add a few tags so people find it, lowercase words joined by hyphens, for example [\"lo-fi\", \"chorus\"]")}
if (typeof meta.license !== "string" || !/^[A-Za-z0-9.+-]+$/.test(meta.license)) {problems.push("meta.json: license, an SPDX id such as MIT")}
if (typeof meta.credits !== "string" || meta.credits.length > 500) {problems.push("meta.json: credits, at most 500 characters")}
for (const key of Object.keys(meta)) {if (!allowedMeta.has(key)) {problems.push(`meta.json: "${key}" is not allowed`)}}
for (const entry of fs.readdirSync(folder)) {
    if (entry.startsWith(".")) {continue}
    if (entry !== "meta.json" && entry !== "source") {problems.push(`${slug}/${entry}: only meta.json and source/ belong in the folder`)}
}
const walk = directory => fs.readdirSync(directory, {withFileTypes: true}).flatMap(entry =>
    entry.isDirectory() ? walk(path.join(directory, entry.name)) : [path.join(directory, entry.name)])
const sources = walk(path.join(folder, "source"))
const size = sources.reduce((sum, file) => sum + fs.statSync(file).size, 0)
if (size > 20 * 1024 * 1024) {problems.push(`source/ is ${(size / 1024 / 1024).toFixed(1)} MB, at most 20 MB`)}
for (const file of sources) {
    if (/\.(wasm|tar\.gz|tgz|o|a)$/.test(file)) {problems.push(`${path.relative(folder, file)}: only source code, never a built file`)}
}
if (problems.length > 0) {fail(`Fix these first:\n${problems.map(problem => `  - ${problem}`).join("\n")}`)}

// the plugin must build and pass the checks, as the repository's check will build it again
for (const script of ["build.mjs", "check.mjs"]) {
    const result = run(process.execPath, [path.join(scripts, script), folder], {stdio: "inherit"})
    if (result.status !== 0) {fail(`${script} did not pass. Fix that before sharing.`)}
}

const login = requireGitHub()
const existing = findPullRequest(login)
if (!confirmed) {
    console.log(`\nReady to share "${meta.name}". With --yes this will:`)
    console.log(`  1. fork ${COMMONS_REPO} to github.com/${login} (once)`)
    console.log(`  2. put this folder into wclaps/${slug} on the branch ${branch} of the fork`)
    console.log(existing.length > 0 ? `  3. update the open pull request ${existing}` : `  3. open a pull request "WebCLAP: ${meta.name}" to ${COMMONS_REPO}`)
    console.log("  (git on this computer will use the GitHub sign-in of gh: gh auth setup-git)")
    console.log("Everything there is public. Ask the user, then run again with --yes.")
    process.exit(0)
}

// --- fork, branch, push, pull request ---------------------------------------------------------------------------

const step = (label, command, args, options = {}) => {
    const result = run(command, args, options)
    if (result.status !== 0) {fail(`${label} failed:\n${`${result.stdout}${result.stderr}`.trim()}`)}
    return result.stdout.trim()
}
if (meta.author.github === undefined) {
    meta.author.github = login
    fs.writeFileSync(path.join(folder, "meta.json"), `${JSON.stringify(meta, null, 2)}\n`)
}
// the fork, which GitHub creates in the background: wait for it, and push only to a real fork of opendaw-commons
const sleep = seconds => Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, seconds * 1000)
const owner = COMMONS_REPO.split("/")[0]
const fork = `${login}/opendaw-commons`
if (login !== owner) {
    gh(["repo", "fork", COMMONS_REPO, "--clone=false"])
    let parent = ""
    for (let attempt = 0; attempt < 12 && parent !== COMMONS_REPO; attempt++) {
        if (attempt > 0) {sleep(5)}
        const result = gh(["api", `repos/${fork}`, "--jq", "if .fork then .parent.full_name else \"\" end"])
        parent = result.status === 0 ? result.stdout.trim() : ""
    }
    if (parent !== COMMONS_REPO) {
        fail(`github.com/${fork} is not a fork of ${COMMONS_REPO} (or GitHub is still creating it). If you have a repository of that name, rename it, then run this again.`)
    }
}
step("Setting up git with the GitHub account", "gh", ["auth", "setup-git"])
const work = path.join(buildsOf(folder), ".commons")
fs.rmSync(work, {recursive: true, force: true})
step("Downloading opendaw-commons", "git", ["clone", "--quiet", "--depth", "1", `https://github.com/${COMMONS_REPO}.git`, work])
const git = (label, args) => step(label, "git", ["-C", work, ...args])
git("Creating the branch", ["checkout", "--quiet", "-b", branch])
const target = path.join(work, "wclaps", slug)
fs.rmSync(target, {recursive: true, force: true})
fs.mkdirSync(target, {recursive: true})
fs.copyFileSync(path.join(folder, "meta.json"), path.join(target, "meta.json"))
fs.cpSync(path.join(folder, "source"), path.join(target, "source"), {recursive: true, filter: source => !path.basename(source).startsWith(".")})
const id = gh(["api", "user", "--jq", ".id"]).stdout.trim()
git("Naming the commit's author", ["config", "user.name", meta.author.name])
git("Naming the commit's author", ["config", "user.email", `${id}+${login}@users.noreply.github.com`])
git("Adding the plugin", ["add", "--all", `wclaps/${slug}`])
const pending = run("git", ["-C", work, "diff", "--cached", "--quiet"]).status !== 0
if (pending) {git("Committing", ["commit", "--quiet", "-m", `WebCLAP: ${meta.name}`])}
let pushed = null
for (let attempt = 0; attempt < 3 && pushed?.status !== 0; attempt++) {
    if (attempt > 0) {sleep(5)}
    pushed = run("git", ["-C", work, "push", "--quiet", "--force", `https://github.com/${fork}.git`, `HEAD:${branch}`])
}
if (pushed.status !== 0) {fail(`Pushing to the fork failed:\n${`${pushed.stdout}${pushed.stderr}`.trim()}`)}
fs.rmSync(work, {recursive: true, force: true})
if (existing.length > 0) {
    console.log(`✓ Updated the pull request: ${existing}`)
} else {
    const body = `${meta.description}\n\nA WebCLAP made with the openDAW WebCLAP kit (built and checked locally before this pull request).`
    const url = step("Opening the pull request", "gh", ["pr", "create", "--repo", COMMONS_REPO, "--base", "main",
        "--head", `${login}:${branch}`, "--title", `WebCLAP: ${meta.name}`, "--body", body])
    console.log(`✓ Opened the pull request: ${url}`)
}
console.log("The repository's check runs now (a few minutes) and comments. See it with --status. A maintainer reviews every plugin before it goes live.")
