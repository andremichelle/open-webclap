#!/usr/bin/env python3
"""Generate the Aether WebCLAP engine sources from the pinned faust-aether checkout.

Runs the pinned Faust compiler (2.88.0 at the time of writing; FAUST env var or faust on PATH, or the first
argument) on external/faust-aether's aether.dsp and effects.dsp and writes into the output directory:

  voice_gen.cpp   aether.dsp as a C++ class AetherVoice (single precision, like the published web build)
  fx_gen.cpp      effects.dsp as a C++ class AetherFX (the shared stereo bus, class name kept distinct)
  params.inc      the parameter table: a C++ ParamSpec array, the same table as JSON (the page protocol's
                  describe message), and AETHER_VERSION from the .dsp's declare version
  patches.json    upstream's presets/patches.json with short names resolved to full Faust addresses, plus the
                  presentation strings from upstream's web/app.js (MIT, same project), for the bundle's page

Usage: gen.py <external/faust-aether dir> <output dir> [faust binary]
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

NOTE_CONTROLS = ("/Aether/freq", "/Aether/gain", "/Aether/gate")

# Presentation strings copied from upstream web/app.js (same MIT project), so the bundle's preset cards read
# like the published web version.
PRESET_INFO = {
    "nebula": ("Nebula", "PAD / ANALOG", "A slow, luminous pad with an expansive sense of space."),
    "glass": ("Glass", "KEYS / FM", "Shards of glass, blending FM bells with inharmonic overtones."),
    "acid": ("Acid", "SEQUENCE / BASS", "A biting filter. Sixteen steps. A restless bass line."),
    "dark_brass": ("Dark Brass", "BRASS / HYBRID", "A warm, dense breath shaped by a moving filter."),
    "orbital": ("Orbital", "TEXTURE / EXPERIMENTAL", "Metal, ring modulation, and weightless echoes."),
    "crystal_sequence": ("Crystal Sequence", "SEQUENCE / DIGITAL", "A crystalline arpeggio unfolding across a stereo space."),
}

MENU_RE = re.compile(r"'([^']*)':\s*(-?[\d.]+)")


def leaves(ui, path=()):
    """Flatten a Faust JSON UI tree into (type, label, address, init, min, max, step, group, meta) tuples."""
    out = []
    for item in ui:
        if "items" in item:
            out += leaves(item["items"], path + (item.get("label", "?"),))
        else:
            meta = {}
            for entry in item.get("meta") or []:
                if isinstance(entry, dict) and len(entry) == 1:
                    key, value = next(iter(entry.items()))
                    meta[key] = value
            out.append((item.get("type"), item.get("label"), item.get("address"), item.get("init"),
                        item.get("min"), item.get("max"), item.get("step"), "/".join(path[1:]), meta))
    return out


def menu_items(meta):
    style = meta.get("style", "")
    if not style.startswith("menu{"):
        return None
    return [(name, float(value)) for name, value in MENU_RE.findall(style[len("menu{"):-1])]


def run_faust(faust, work, args, cwd):
    result = subprocess.run([faust, *args], cwd=cwd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"faust {' '.join(args)} failed:\n{result.stderr}")
    if result.stderr.strip():
        print(faust, " ".join(args), "->", result.stderr.strip(), file=sys.stderr)


def main():
    src, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    faust = sys.argv[3] if len(sys.argv) > 3 else os.environ.get("FAUST") or shutil.which("faust")
    if not faust:
        for candidate in (os.path.expanduser("~/faust/bin/faust"), "/usr/local/bin/faust", "/opt/homebrew/bin/faust"):
            if os.path.exists(candidate):
                faust = candidate
                break
    if not faust or not os.path.exists(faust):
        raise SystemExit("faust not found: pass its path, set FAUST, or install the Faust 2.88.0 release "
                         "(https://github.com/grame-cncm/faust/releases) to ~/faust")
    os.makedirs(out, exist_ok=True)

    with tempfile.TemporaryDirectory() as work:
        for name in ("aether.dsp", "effects.dsp"):
            shutil.copy(os.path.join(src, name), work)
        run_faust(faust, work, ["-lang", "cpp", "-cn", "AetherVoice", "-I", work, "aether.dsp",
                                "-o", os.path.join(out, "voice_gen.cpp")], work)
        run_faust(faust, work, ["-lang", "cpp", "-cn", "AetherFX", "-I", work, "effects.dsp",
                                "-o", os.path.join(out, "fx_gen.cpp")], work)
        run_faust(faust, work, ["--json", "-I", work, "aether.dsp"], work)
        run_faust(faust, work, ["--json", "-I", work, "effects.dsp"], work)
        voice_json = json.load(open(os.path.join(work, "aether.dsp.json")))
        fx_json = json.load(open(os.path.join(work, "effects.dsp.json")))

    version = re.search(r'declare version "([^"]+)"', open(os.path.join(src, "aether.dsp")).read()).group(1)

    # The parameter table: the voice widgets (minus the note controls freq/gain/gate) as ids 0.., then the
    # effect bus widgets (minus its copy of bpm, which the voice id drives on both instances).
    params = []

    def add(leaf, fx=False):
        widget, label, address, init, lo, hi, step, group, meta = leaf
        if widget == "button":
            return
        if widget == "checkbox":
            lo, hi, step, init = 0.0, 1.0, 1.0, float(init or 0)
        menu = menu_items(meta)
        params.append({
            "id": len(params), "key": address, "label": label, "group": group, "type": widget,
            "min": float(lo), "max": float(hi), "step": float(step), "init": float(init if init is not None else lo),
            "unit": meta.get("unit"), "log": meta.get("scale") == "log",
            "menu": menu, "tooltip": meta.get("tooltip"), "fx": fx,
        })

    for leaf in leaves(voice_json["ui"]):
        if leaf[2] not in NOTE_CONTROLS:
            add(leaf)
    bpm_id = next(p["id"] for p in params if p["key"].endswith("/bpm"))
    for leaf in leaves(fx_json["ui"]):
        if leaf[2].endswith("/bpm"):
            continue
        add(leaf, fx=True)

    # --- params.inc -------------------------------------------------------------------------------------------------
    def cstr(text):
        return json.dumps(text)  # JSON string escaping is valid C++ for these ASCII strings

    def menu_c(menu):
        if not menu:
            return "nullptr, 0"
        entries = ", ".join(f"{{{cstr(name)}, {value:g}}}" for name, value in menu)
        return f"(const MenuEntry[]){{{entries}}}, {len(menu)}"

    lines = ["// generated by gen.py from the pinned faust-aether checkout — do not edit",
             f'#define AETHER_VERSION "{version}"', "",
             "struct MenuEntry { const char* name; double value; };", "",
             "struct ParamSpec {", "    clap_id id; const char* key; const char* name; const char* group;",
             "    const char* type; double min, max, defaultValue, step; const char* unit; bool logarithmic;",
             "    const MenuEntry* menu; uint32_t menuCount; const char* tooltip; bool fx;", "};", "",
             "constexpr ParamSpec paramSpecs[] = {"]
    for p in params:
        menu = menu_c(p["menu"])
        if p["menu"] is None and p["tooltip"] == "0=ladder 1=bandpass 2=highpass":
            menu = menu_c([("Ladder", 0), ("Band-pass", 1), ("High-pass", 2)])
        lines.append(
            f"    {{{p['id']}, {cstr(p['key'])}, {cstr(p['label'])}, {cstr(p['group'])}, {cstr(p['type'])}, "
            f"{p['min']:g}, {p['max']:g}, {p['init']:g}, {p['step']:g}, {cstr(p['unit']) if p['unit'] else 'nullptr'}, "
            f"{'true' if p['log'] else 'false'}, {menu}, {cstr(p['tooltip']) if p['tooltip'] else 'nullptr'}, "
            f"{'true' if p['fx'] else 'false'}}},")
    lines += ["};", f"constexpr clap_id paramCount = {len(params)};", f"constexpr clap_id bpmParamId = {bpm_id};", ""]

    table = [{k: p[k] for k in ("id", "key", "label", "group", "type", "min", "max", "step", "init", "unit",
                               "log", "menu", "tooltip")} for p in params]
    lines.append("static const char paramTableJson[] = R\"AETHERJSON(" + json.dumps(table) + ")AETHERJSON\";")
    open(os.path.join(out, "params.inc"), "w").write("\n".join(lines) + "\n")

    # --- patches.json for the bundle's page -------------------------------------------------------------------------
    by_label = {}
    for p in params:
        by_label.setdefault(p["label"], p)
    patches = json.load(open(os.path.join(src, "presets", "patches.json")))
    out_patches = {}
    for name, values in patches.items():
        resolved = {}
        for key, value in values.items():
            if key.startswith("Route_"):
                address = "/Aether/Voice/Modulation/" + key.replace("/", "/")
                param = next(p for p in params if p["key"] == address)
            else:
                param = by_label[key]
            resolved[param["key"]] = value
        title, tag, description = PRESET_INFO.get(name, (name, "", ""))
        out_patches[name] = {"title": title, "tag": tag, "description": description, "values": resolved}
    open(os.path.join(out, "patches.json"), "w").write(json.dumps(out_patches, indent=1) + "\n")

    print(f"gen.py: {len(params)} params (voice 0..{bpm_id}.., fx tail), version {version}, "
          f"{len(out_patches)} presets -> {out}")


if __name__ == "__main__":
    main()
