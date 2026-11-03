#!/usr/bin/env python3
"""Helm's factory patches as one JSON file for the editor: patches.py <external/helm> <patches.json>

Each .helm file is a patch of some Helm version; this brings it to the current one as upstream's
LoadSave::varToState does (src/common/load_save.cpp), then keeps only the values that differ from the defaults
(src/common/helm_common.cpp). The patches are CC BY 4.0, every entry keeps its author.

  {"patches": [{"name", "folder", "author", "values": {control: value}, "modulations": [[source, destination,
  amount]]}]}
"""

import json
import math
import os
import re
import sys


def version(text):
    return tuple(int(part) for part in re.findall(r"\d+", text)[:3])


def defaults(helm):
    table = open(os.path.join(helm, "src/common/helm_common.cpp")).read()
    rows = re.findall(r'\{\s*"(\w+)",\s*([-\d.e]+),\s*([-\d.e]+),\s*(\d+),\s*([-\d.e]+),', table)
    return {name: float(default) for name, _, _, _, default in rows}


# LoadSave::varToState's fix-ups for patches of older versions, in order
def migrate(patch):
    synth_version = version(patch.get("synth_version", "0.4.1"))
    settings = dict(patch["settings"] if synth_version > (0, 4, 1) else patch)
    settings.pop("settings", None)
    modulations = [dict(m) for m in settings.pop("modulations", [])]
    get = lambda name: float(settings.get(name, 0.0))

    if synth_version <= (0, 5, 0):
        if "osc_mix" in settings:
            osc_mix = get("osc_mix")
            settings["osc_1_volume"] = math.sqrt(1.0 - osc_mix)
            settings["osc_2_volume"] = math.sqrt(osc_mix)
            del settings["osc_mix"]
        renamed = []
        for m in modulations:
            if m["destination"] == "osc_mix":
                renamed += [{"source": m["source"], "destination": "osc_1_volume", "amount": -m["amount"]},
                            {"source": m["source"], "destination": "osc_2_volume", "amount": m["amount"]}]
            else:
                renamed.append(m)
        modulations = renamed

    if synth_version <= (0, 7, 2) and get("stutter_on"):
        settings["stutter_resample_sync"] = 0
        settings["stutter_sync"] = 0

    if synth_version <= (0, 8, 6):
        voices1, voices2 = get("osc_1_unison_voices"), get("osc_2_unison_voices")
        old_volume1, old_volume2 = get("osc_1_volume") ** 2, get("osc_2_volume") ** 2
        settings["osc_1_volume"] = math.sqrt(old_volume1 * math.sqrt(0.5 / ((voices1 + 1.0) / 2.0)))
        settings["osc_2_volume"] = math.sqrt(old_volume2 * math.sqrt(0.5 / ((voices2 + 1.0) / 2.0)))
        settings["sub_volume"] = math.sqrt(0.5) * get("sub_volume")

        if synth_version <= (0, 5, 0):
            settings["sub_octave"] = 1.0
            settings["cutoff"] = get("cutoff") - get("keytrack") * 12.0

        filter_type = get("filter_type")
        if filter_type >= 6.0:
            settings["filter_on"] = 0.0
        elif filter_type >= 3.0:
            settings["filter_shelf"] = 1.0 if filter_type >= 5.0 else 2.0 if filter_type >= 4.0 else 0.0
            settings["filter_on"] = 1.0
            settings["filter_style"] = 2.0
        else:
            if filter_type >= 2.0:
                settings["filter_blend"] = 1.0
            elif filter_type >= 1.0:
                settings["filter_blend"] = 2.0
            settings["filter_on"] = 1.0
            settings["filter_style"] = 0.0

        settings["distortion_on"] = 1.0
        settings["distortion_type"] = 0.0
        settings["distortion_mix"] = 1.0
        saturation = get("filter_saturation")
        if filter_type >= 6.0:
            settings["distortion_drive"] = saturation
        else:
            settings["distortion_drive"] = saturation + 12.0
            settings["filter_drive"] = -12.0
        for m in modulations:
            if m["destination"] == "filter_saturation":
                m["destination"] = "distortion_drive"

        volume = get("volume")
        for effect in ("delay", "reverb"):
            wet, on = get(effect + "_dry_wet"), get(effect + "_on")
            if on and wet != 0.0 and wet != 1.0:
                ratio = wet / (1.0 - wet)
                new_wet = 1.0 - 1.0 / (1.0 + ratio * ratio)
                settings[effect + "_dry_wet"] = new_wet
                volume *= math.sqrt(wet / math.sqrt(new_wet))
        settings["volume"] = volume
        settings["beats_per_minute"] = get("beats_per_minute") / 60.0

    return settings, modulations


def main(helm, out):
    known = defaults(helm)
    root = os.path.join(helm, "patches", "Factory Presets")
    patches = []
    for folder in sorted(os.listdir(root)):
        directory = os.path.join(root, folder)
        if not os.path.isdir(directory):
            continue
        for file in sorted(os.listdir(directory), key=str.lower):
            if not file.endswith(".helm"):
                continue
            patch = json.load(open(os.path.join(directory, file)))
            settings, modulations = migrate(patch)
            values = {name: round(float(value), 6) for name, value in sorted(settings.items())
                      if name in known and isinstance(value, (int, float)) and round(float(value), 6) != known[name]}
            patches.append({
                "name": file[:-len(".helm")],
                "folder": folder,
                "author": (patch.get("author") or "").strip(),
                "values": values,
                "modulations": [[m["source"], m["destination"], round(float(m["amount"]), 6)]
                                for m in modulations if float(m["amount"]) != 0.0],
            })
    with open(out, "w") as file:
        json.dump({"patches": patches}, file, separators=(",", ":"))
    print(f"{len(patches)} patches, {os.path.getsize(out) // 1024} KB")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
