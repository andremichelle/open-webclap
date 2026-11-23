// Installs what building a WebCLAP needs, once, into ~/.opendaw-webclap: Emscripten and the CLAP headers, the
// versions opendaw-commons' check builds with. An Emscripten of that version already on the path is used as is.
//
//     node scripts/setup.mjs           install (does nothing when everything is there)
//     node scripts/setup.mjs --check   only check the installed toolchain
import fs from "node:fs"
import path from "node:path"
import {CLAP_COMMIT, EMSDK_VERSION, HOME, cleanEnv, emccVersion, fail, has, readToolchain, run, toolchainEnv, toolchainFile} from "./lib.mjs"

const checkOnly = process.argv.includes("--check")

const step = text => console.log(`\n→ ${text}`)
const ok = text => console.log(`✓ ${text}`)

const check = toolchain => {
    const env = toolchainEnv(toolchain)
    const version = emccVersion(env)
    if (version !== EMSDK_VERSION) {return `Emscripten ${EMSDK_VERSION} is needed, found ${version ?? "none"}.`}
    if (!fs.existsSync(path.join(toolchain.clapInclude, "clap", "clap.h"))) {return `The CLAP headers are missing in ${toolchain.clapInclude}.`}
    return null
}

if (checkOnly) {
    const toolchain = readToolchain()
    if (toolchain === null) {fail("Not installed yet. Run: node scripts/setup.mjs")}
    const problem = check(toolchain)
    if (problem !== null) {fail(`${problem} Run: node scripts/setup.mjs`)}
    ok(`Toolchain ready: Emscripten ${EMSDK_VERSION}, CLAP headers ${CLAP_COMMIT.slice(0, 7)}.`)
    process.exit(0)
}

if (process.platform === "win32") {
    fail("On Windows, run the agent inside WSL (Windows Subsystem for Linux): https://learn.microsoft.com/windows/wsl/install")
}
const [major] = process.versions.node.split(".").map(Number)
if (major < 20) {fail(`Node 20 or later is needed, this is ${process.versions.node}. Get it from https://nodejs.org`)}
for (const tool of ["git", "python3", "bash"]) {
    if (!has(tool)) {
        fail(process.platform === "darwin" && tool !== "bash"
            ? `${tool} is missing. Run: xcode-select --install (macOS asks once to install its command line tools)`
            : `${tool} is missing. Install it with your system's package manager (for example: sudo apt install ${tool}).`)
    }
}
fs.mkdirSync(HOME, {recursive: true})
// one setup at a time (two agents may start at once): the second waits for the first
const lock = path.join(HOME, ".setup-lock")
const sleep = seconds => Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, seconds * 1000)
for (let waited = 0; ; waited += 2) {
    try {
        fs.mkdirSync(lock)
        break
    } catch (_) {
        const age = fs.existsSync(lock) ? (Date.now() - fs.statSync(lock).mtimeMs) / 1000 : 0
        if (age > 30 * 60) {fs.rmSync(lock, {recursive: true, force: true})}
        if (waited === 0) {console.log("→ Another setup is running, waiting for it")}
        sleep(2)
    }
}
process.on("exit", () => fs.rmSync(lock, {recursive: true, force: true}))

// Emscripten: the one on the path if it has the right version, else our own emsdk
let emsdk = null
const pathVersion = emccVersion(process.env)
if (pathVersion === EMSDK_VERSION) {
    ok(`Emscripten ${EMSDK_VERSION} found on the path.`)
} else {
    emsdk = path.join(HOME, "emsdk")
    const installed = path.join(emsdk, "upstream", "emscripten", "emscripten-version.txt")
    const current = fs.existsSync(installed) ? fs.readFileSync(installed, "utf8").replace(/["\s]/g, "") : null
    if (current === EMSDK_VERSION) {
        ok(`Emscripten ${EMSDK_VERSION} is installed.`)
    } else {
        // an emsdk of another release (an interrupted or older install) goes
        const tag = fs.existsSync(path.join(emsdk, ".git")) ? run("git", ["-C", emsdk, "describe", "--tags", "--exact-match"]).stdout.trim() : null
        if (tag !== EMSDK_VERSION) {fs.rmSync(emsdk, {recursive: true, force: true})}
        if (!fs.existsSync(emsdk)) {
            step("Downloading the Emscripten SDK")
            // the emsdk of that release: newer ones want a newer Python than macOS ships
            const clone = run("git", ["clone", "--quiet", "--depth", "1", "--branch", EMSDK_VERSION,
                "https://github.com/emscripten-core/emsdk.git", emsdk], {stdio: "inherit"})
            if (clone.status !== 0) {fail("Could not download the Emscripten SDK. Is the computer online?")}
        }
        step(`Installing Emscripten ${EMSDK_VERSION} (about 1 GB, a few minutes)`)
        const install = run("./emsdk", ["install", EMSDK_VERSION], {cwd: emsdk, stdio: "inherit", env: cleanEnv()})
        if (install.status !== 0) {fail("Installing Emscripten failed, see the lines above.")}
        const activate = run("./emsdk", ["activate", EMSDK_VERSION], {cwd: emsdk, stdio: "inherit", env: cleanEnv()})
        if (activate.status !== 0) {fail("Activating Emscripten failed, see the lines above.")}
        ok(`Emscripten ${EMSDK_VERSION} installed.`)
    }
}

// the CLAP headers at the commit opendaw-commons builds with
const clap = path.join(HOME, "clap")
const clapHeader = path.join(clap, "include", "clap", "clap.h")
const clapHead = fs.existsSync(path.join(clap, ".git")) ? run("git", ["rev-parse", "HEAD"], {cwd: clap}).stdout.trim() : null
if (clapHead === CLAP_COMMIT && fs.existsSync(clapHeader)) {
    ok("CLAP headers are installed.")
} else {
    step("Downloading the CLAP headers")
    fs.rmSync(clap, {recursive: true, force: true})
    fs.mkdirSync(clap, {recursive: true})
    const git = args => run("git", args, {cwd: clap})
    const steps = [["init", "-q"], ["remote", "add", "origin", "https://github.com/free-audio/clap.git"],
        ["fetch", "-q", "--depth", "1", "origin", CLAP_COMMIT], ["checkout", "-q", "FETCH_HEAD"]]
    for (const args of steps) {
        const result = git(args)
        if (result.status !== 0) {fail(`Could not download the CLAP headers: ${result.stderr.trim()}`)}
    }
    ok("CLAP headers installed.")
}

const toolchain = {emsdk, clapInclude: path.join(clap, "include")}
const problem = check(toolchain)
if (problem !== null) {fail(problem)}
fs.writeFileSync(toolchainFile, JSON.stringify(toolchain, null, 2))
ok(`Ready. The toolchain lives in ${HOME}`)
