#!/usr/bin/env python3
"""Generate the Elements WebCLAP engine sources from the pinned faust-elements checkout.

Runs the pinned faust-rs compiler (grame-cncm/faust-rs; elements.lib uses `ondemand`, which upstream Faust does
not have) on this port's src/voice.dsp and src/body.dsp — the voice and the effect bus of upstream's
dsp/elements_poly.dsp, verbatim — single precision, and writes into the output directory:

  voice_gen.cpp   src/voice.dsp as the self-contained C++ class ElementsVoice (0 inputs, 3 outputs: raw, center, side)
  body_gen.cpp    src/body.dsp as the self-contained C++ class ElementsBody (3 inputs, 2 outputs: main, aux)
  params.inc      the parameter table: a C++ ParamSpec array and the same table as JSON (the page protocol's
                  describe message). 21 parameters: the 18 panel controls of the voice, then the 3 of the body
  patches.json    upstream's 24 web presets (extracted from web/controls.js by the build script), resolved to
                  full Faust addresses, for the bundle's page

Usage: gen.py <external/faust-elements dir> <output dir> <faust-rs binary> <faust stdlib dir> [presets.json]
"""

import json
import os
import subprocess
import sys

# The note controls of the voice: driven by MIDI, not parameters.
NOTE_CONTROLS = ("/elements_poly/freq", "/elements_poly/gain", "/elements_poly/gate")

# Panel order of the voice's groups, as on the module.
GROUP_RANK = {"exciter": 0, "resonator": 1, "unit": 2, "performance": 3}

# The resonator model is a whole number 0..2; give it a menu.
MODEL_MENU = [("Modal", 0), ("String", 1), ("Strings", 2)]


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


def run_faustrs(faustrs, args, cwd):
    result = subprocess.run([faustrs, *args], cwd=cwd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"faust-rs {' '.join(args)} failed:\n{result.stderr[-4000:]}")
    if result.stderr.strip():
        print("faust-rs", " ".join(args), "->", result.stderr.strip()[-500:], file=sys.stderr)
    return result.stdout


def main():
    src, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    faustrs = os.path.abspath(sys.argv[3])
    faust_lib = os.path.abspath(sys.argv[4])
    presets_path = os.path.abspath(sys.argv[5]) if len(sys.argv) > 5 else None
    port_src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "src")
    dsp_dir = os.path.join(src, "dsp")
    os.makedirs(out, exist_ok=True)

    includes = ["-I", dsp_dir, "-I", faust_lib]
    for name, cls in (("voice", "ElementsVoice"), ("body", "ElementsBody")):
        run_faustrs(faustrs, ["-lang", "cpp", "-cn", cls, "-single", "--timeout", "0",
                              *includes, os.path.join(port_src, name + ".dsp"),
                              "-o", os.path.join(out, name + "_gen.cpp")], out)
        json_text = run_faustrs(faustrs, ["--json", *includes, os.path.join(port_src, name + ".dsp")], out)
        with open(os.path.join(out, name + ".json"), "w") as handle:
            handle.write(json_text)

    voice_json = json.load(open(os.path.join(out, "voice.json")))
    body_json = json.load(open(os.path.join(out, "body.json")))

    # --- orbit.json: the ui descriptor for the editor's Orbit tab (faust-orbit-ui), mirroring upstream
    # web/orbit.js's descriptor(): the voice's and the body's groups side by side in one vgroup, minus the
    # note controls and the pitch offset (the keyboard and the bend own those) ---------------------------
    excluded = {"/elements_poly/freq", "/elements_poly/gain", "/elements_poly/gate",
                "/elements_poly/performance/modulation"}

    def keep(items):
        kept = []
        for item in items or []:
            if isinstance(item, dict) and "items" in item:
                kids = keep(item["items"])
                if kids:
                    kept.append({**item, "items": kids})
            elif item.get("address") and item["address"] not in excluded:
                kept.append(item)
        return kept

    def top(dsp_json):
        kept = keep(dsp_json["ui"])
        return [kid for g in kept for kid in (g["items"] if isinstance(g, dict) and "items" in g else [g])]

    orbit_descriptor = [{"type": "vgroup", "label": "elements", "items": [*top(voice_json), *top(body_json)]}]
    with open(os.path.join(out, "orbit.json"), "w") as handle:
        json.dump(orbit_descriptor, handle)

    # --- the parameter table: the voice's panel controls (minus the note controls), in panel order, then the
    # body's -----------------------------------------------------------------------------------------------------
    params = []

    def add(leaf, body=False):
        widget, label, address, init, lo, hi, step, group, meta = leaf
        if widget == "button":
            return
        if widget == "checkbox":
            lo, hi, step, init = 0.0, 1.0, 1.0, float(init or 0)
        menu = None
        if label == "model":
            menu = MODEL_MENU
        params.append({
            "id": len(params), "key": address, "label": label, "group": group, "type": widget,
            "min": float(lo), "max": float(hi), "step": float(step), "init": float(init if init is not None else lo),
            "unit": meta.get("unit"), "log": meta.get("scale") == "log",
            "menu": menu, "tooltip": meta.get("tooltip"), "body": body,
        })

    voice_leaves = [leaf for leaf in leaves(voice_json["ui"]) if leaf[2] not in NOTE_CONTROLS]
    voice_leaves.sort(key=lambda leaf: (GROUP_RANK.get(leaf[7], 9), leaf[2]))
    for leaf in voice_leaves:
        add(leaf)
    for leaf in leaves(body_json["ui"]):
        add(leaf, body=True)
    mod_id = next(p["id"] for p in params if p["key"].endswith("/modulation"))

    # --- params.inc ----------------------------------------------------------------------------------------------
    def cstr(text):
        return json.dumps(text)  # JSON string escaping is valid C++ for these ASCII strings

    def menu_c(menu):
        if not menu:
            return "nullptr, 0"
        entries = ", ".join(f"{{{cstr(name)}, {value:g}}}" for name, value in menu)
        return f"(const MenuEntry[]){{{entries}}}, {len(menu)}"

    lines = ["// generated by gen.py from the pinned faust-elements checkout — do not edit",
             '#define ELEMENTS_VERSION "1.0.0"', "",
             "struct MenuEntry { const char* name; double value; };", "",
             "struct ParamSpec {", "    clap_id id; const char* key; const char* name; const char* group;",
             "    const char* type; double min, max, defaultValue, step; const char* unit; bool logarithmic;",
             "    const MenuEntry* menu; uint32_t menuCount; const char* tooltip; bool body;", "};", "",
             "constexpr ParamSpec paramSpecs[] = {"]
    for p in params:
        menu = menu_c(p["menu"])
        lines.append(
            f"    {{{p['id']}, {cstr(p['key'])}, {cstr(p['label'])}, {cstr(p['group'])}, {cstr(p['type'])}, "
            f"{p['min']:g}, {p['max']:g}, {p['init']:g}, {p['step']:g}, {cstr(p['unit']) if p['unit'] else 'nullptr'}, "
            f"{'true' if p['log'] else 'false'}, {menu}, {cstr(p['tooltip']) if p['tooltip'] else 'nullptr'}, "
            f"{'true' if p['body'] else 'false'}}},")
    lines += ["};", f"constexpr clap_id paramCount = {len(params)};", f"constexpr clap_id modulationParamId = {mod_id};", ""]

    table = [{k: p[k] for k in ("id", "key", "label", "group", "type", "min", "max", "step", "init", "unit",
                               "log", "menu", "tooltip", "body")} for p in params]
    lines.append("static const char paramTableJson[] = R\"ELEMJSON(" + json.dumps(table) + ")ELEMJSON\";")
    open(os.path.join(out, "params.inc"), "w").write("\n".join(lines) + "\n")

    # --- patches.json for the bundle's page ----------------------------------------------------------------------
    if presets_path:
        presets = json.load(open(presets_path))  # [{id, tag, title, description, values: {control key: value}}]
        out_patches = {}
        for p in presets:
            values = {}
            for key, value in p["values"].items():
                address = "/elements_poly/" + key
                if not any(q["key"] == address for q in params):
                    raise RuntimeError(f"preset {p['id']}: unknown control {key}")
                values[address] = value
            out_patches[p["id"]] = {"title": p["title"], "tag": p.get("tag", ""),
                                    "description": p.get("description", ""), "values": values}
        open(os.path.join(out, "patches.json"), "w").write(json.dumps(out_patches, indent=1) + "\n")
        print(f"gen.py: {len(params)} params (voice 0..{mod_id}.., body tail), {len(out_patches)} presets -> {out}")
    else:
        print(f"gen.py: {len(params)} params -> {out} (no presets.json given)")


if __name__ == "__main__":
    main()
