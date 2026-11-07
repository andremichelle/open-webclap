#!/usr/bin/env python3
"""Convert the pinned upstream's patch banks (the ones its own installers ship: Shared/Resources/Synth2 Factory and
Shared/Resources/Synth2 Starter Banks/*) into the one patches.json the bundle's page loads from.

The .sy1 format: line 1 = name, key=value headers (color, ver), then "index,value" lines of native values, CRLF
endings included. The version is carried through; the plugin runs the engine's legacy conversion when ver < 112
(both shipped banks are ver 112/113, so this is for future banks). Values pass through unclamped; the plugin
clamps on receipt. Usage: patches.py <external/oxo-synth2> <output patches.json>
"""

import json
import os
import sys


def parse_sy1(path):
    with open(path, "rb") as f:
        text = f.read().decode("utf-8", "replace")
    lines = text.splitlines()
    name = lines[0] if lines else os.path.basename(path)
    version = 0
    values = [None] * 99
    for line in lines[1:]:
        if "=" in line and "," not in line.split("=")[0]:
            key, _, value = line.partition("=")
            if key.strip() == "ver":
                try:
                    version = int(value.strip())
                except ValueError:
                    pass
            continue
        if "," in line:
            index, _, value = line.partition(",")
            try:
                i, v = int(index.strip()), int(value.strip())
            except ValueError:
                continue
            if 0 <= i < 99:
                values[i] = v
    return name, version, values


def main():
    source, output = sys.argv[1], sys.argv[2]
    resources = os.path.join(source, "Shared", "Resources")
    banks = []
    # the factory bank first (its 001 is the INIT patch), then the starter banks
    for folder in ("Synth2 Factory", "Synth2 Starter Banks"):
        root = os.path.join(resources, folder)
        if not os.path.isdir(root):
            continue
        subfolders = sorted(d for d in os.listdir(root) if os.path.isdir(os.path.join(root, d)))
        if subfolders:  # starter banks: one bank per subfolder
            for bank in subfolders:
                patches = collect(os.path.join(root, bank), bank)
                if patches:
                    banks.append({"name": bank, "patches": patches})
        else:
            patches = collect(root, folder)
            if patches:
                banks.append({"name": folder, "patches": patches})
    with open(output, "w") as f:
        json.dump({"banks": banks}, f, ensure_ascii=False, separators=(",", ":"))
    total = sum(len(b["patches"]) for b in banks)
    print(f"{output}: {total} patches in {len(banks)} banks ({', '.join(b['name'] for b in banks)})")


def collect(folder, bank):
    patches = []
    for file in sorted(os.listdir(folder)):
        if file.endswith(".sy1"):
            name, version, values = parse_sy1(os.path.join(folder, file))
            patches.append({"name": name, "ver": version, "values": values})
    return patches


if __name__ == "__main__":
    main()
