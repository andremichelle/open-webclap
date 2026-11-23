---
name: opendaw-webclap
description: Build audio plugins (WebCLAP) for openDAW from a plain description - audio effects, instruments and MIDI effects that run in the browser, in openDAW's device panel like a stock device and in their own window. Use when the user wants to make, change, test or share an openDAW plugin, a WebCLAP, or "an effect / synth / MIDI effect for openDAW", even without coding knowledge.
---

# openDAW WebCLAP plugins

You build a plugin for someone who may never read code. They describe a sound or an idea, you build it, they listen
in the preview and ask for changes, and at the end they use it in openDAW Studio and may share it with everyone.
Talk in plain words, never show build output or code unless asked, and decide technical details yourself.

A plugin is a folder laid out exactly as the community repository opendaw-commons wants it:

```
<slug>/meta.json            name, description, author, tags, license, credits
<slug>/source/plugin.cpp    the whole plugin: parameters and sound, on the kit
<slug>/source/page/         the plugin's page: index.html (+ the kit's opendaw-kit.js and opendaw-page/)
<slug>/source/kit/          the kit's C++ headers, never edit them
<slug>/source/build.sh      the build, as opendaw-commons' check runs it, never edit it without a reason
<slug>/source/LICENSE
builds/<slug>.wclap.tar.gz  the built plugin, next to the folder (never inside it)
```

All scripts are in this skill's `scripts/` folder. Run them with node from the folder that holds the plugin folders
(the user's working directory, unless they name another), for example `node <skill>/scripts/build.mjs ./tape-drive`.

## The workflow

1. **Understand the idea.** Kind: audio effect (changes sound passing through), instrument (plays notes), or MIDI
   effect (changes notes before an instrument). Name, the 3 to 8 controls a user would want, their ranges and units.
   Ask at most one short question when the kind is unclear; otherwise decide.
2. **Toolchain, once per computer:** `node scripts/setup.mjs`. It installs Emscripten and the CLAP headers into
   `~/.opendaw-webclap` (about 1 GB, a few minutes the first time; tell the user it is a one time download). Then ask
   the user whether to run a quick toolchain check, they may skip it: `node scripts/setup.mjs --check`.
3. **Start from the template:** `node scripts/new.mjs <audio-effect|instrument|midi-effect> <slug> --name "<Name>"
   --author "<the user's name>" --description "<one or two sentences>" --tags "<tag>,<tag>"`. The slug is lowercase
   words joined by hyphens, it never changes once shared. Tags are what people search for (lo-fi, chorus, bass,
   arpeggiator), lowercase words joined by hyphens, at most 8. Ask the user for the name to put as author if you do
   not know it.
4. **Write the plugin** by editing `source/plugin.cpp` in place: keep its first lines and the INFO values new.mjs
   filled in (clap id, name, vendor, description), replace the parameter table, the feature, and the `Dsp` struct.
   Read `references/kit.md` first, it has the API, the rules and DSP recipes. Only edit `source/page/index.html` for
   a display or an explicit layout.
5. **Build and check** until both are clean: `node scripts/build.mjs <slug>` then `node scripts/check.mjs <slug>`.
   Fix every ✗. Fix ! warnings unless you have a reason not to.
6. **Preview:** `node scripts/preview.mjs <slug>` as a background process (it serves until stopped, one per plugin;
   stop it by ending that process when the user is done). It opens the user's browser on the
   open-webclap site with the plugin in openDAW's engine: the device panel at 176 px above the plugin window, a
   keyboard for instruments, an input (test tone, noise, audio file, microphone) for effects. Tell the user to click
   **Start audio** and try it. Every later build reloads the plugin in the page with its settings kept.
7. **Read the preview's report**: a few seconds after the page loaded in the browser (no click needed) the preview
   prints the panel's measured width and any page error, and writes them to `builds/<slug>.report.json`. Set the
   width in INFO to the measured content width and build again until it says the content fits. Fix any page error.
   Without a browser there is no report: keep the template's width when the layout is close to the template's, and
   tell the user the width is not measured yet.
8. **Iterate** on what the user hears and says: change, build, check. The preview follows by itself.
9. **Use it in openDAW Studio** (https://opendaw.studio): add a WebCLAP device (audio effect, instrument or MIDI
   effect) to a track, click **Load Plugin > Import WebCLAP...** and pick `builds/<slug>.wclap.tar.gz`. Before that,
   raise the version in INFO (`1.0.0` → `1.0.1`) whenever the user had an earlier build in a project.
10. **Share it, only when the user asks.** Needs a free GitHub account. Run `node scripts/share.mjs <slug>` (it
    checks everything and changes nothing), show the user what it will do, and only after they agree run it with
    `--yes`. If `gh` is missing or not signed in, the script says what to run (`gh auth login --web` opens the
    browser for the user to sign in; never type passwords or codes yourself).
    Then `node scripts/share.mjs <slug> --status` shows the check's result on the pull request; fix what it lists,
    build, check and run `--yes` again to update the same pull request. A maintainer reviews every plugin.

## Hard rules (openDAW relies on them)

- One plugin per folder, one `.cpp` file including `opendaw_kit.h`. Plain C++17, no exceptions, no RTTI, no threads,
  no files, no network. Allocate only in `activate`, never in `process`, `noteOn` or `changed`.
- Parameter ids are 0, 1, 2... in table order. Once a plugin is shared, never change an id's meaning, its range or
  its unit, and never remove one: projects and automation point at them. Add new parameters at the end.
- Parameter values are in their units (dB, Hz, ms, %): openDAW automates and types them as they are. A knob curve
  (Exponential for frequencies, Squared for times) only shapes the page's knob.
- Short names: "Res", "Freq", "Mix", "Drive", "Time" (12 characters at most, shorter is better).
- Every sound must start and end without clicks: smooth every gain and frequency per sample (`Smoothed`).
- `reset()` silences everything at once: voices, delay lines, filter states. openDAW resets on stop.
- Flush denormals in every feedback path (`flush()`), WebAssembly cannot.
- No surface of its own on the page: no background, border, title or name (openDAW's device header has the name).
  The page never handles the wheel, double-click, right-click or keys: those are openDAW's.
- Levels last: Mix and Output at the end of the table, so they sit at the end of the panel.
- Raise `version` in INFO with every build the user takes into a project.

## When something fails

- Build errors: read the compiler's first error, fix it in plugin.cpp. The kit's `static_assert` explains a broken
  parameter table.
- `check.mjs` explains each ✗ in its line. "differ at block sizes" means something counts time in calls: compute
  per sample, apply changes in `changed()` or per sample through `Smoothed`.
- The preview page says "not built": build first. It cannot reach the site: the computer is offline.
- On Windows the scripts need WSL: tell the user, link https://learn.microsoft.com/windows/wsl/install.
- `check.mjs` tests what a machine can: loading, parameters, state, timing at any block size, silence after reset,
  speed. Whether it sounds as the user wants is for the user's ears in the preview: ask them.

## Background reading

`references/building-for-opendaw.md` (everything openDAW expects from a WebCLAP, the kit already does it) and
`references/device-panel-design.md` (how panels look). Read them before you write a page of your own beyond a
display.
