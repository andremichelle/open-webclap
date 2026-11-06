#!/usr/bin/env python3
"""Convert the pinned upstream's first-party factory bank (Shared/Resources/Synth2 Factory/*.sy1) into the one
patches.json the bundle's page ships and loads from. The .sy1 format: line 1 = name, key=value headers, then
"index,value" lines of native values (all factory patches are ver=112, so no legacy conversion; the plugin clamps
on receipt). Usage: patches.py <external/oxo-synth2> <output patches.json>
"""

import json
import os
import sys


def parse_sy1(path):
    with open(path, "rb") as f:
        text = f.read().decode("utf-8", "replace")
    lines = text.splitlines()
    name = lines[0] if lines else os.path.basename(path)
    values = [None] * 99
    for line in lines[1:]:
        if "=" in line and "," not in line.split("=")[0]:
            continue  # color=..., ver=...
        if "," in line:
            index, _, value = line.partition(",")
            try:
                i, v = int(index.strip()), int(value.strip())
            except ValueError:
                continue
            if 0 <= i < 99:
                values[i] = v
    return name, values


def main():
    source, output = sys.argv[1], sys.argv[2]
    folder = os.path.join(source, "Shared", "Resources", "Synth2 Factory")
    patches = []
    for file in sorted(os.listdir(folder)):
        if file.endswith(".sy1"):
            name, values = parse_sy1(os.path.join(folder, file))
            patches.append({"name": name, "values": values})
    with open(output, "w") as f:
        json.dump({"patches": patches}, f, ensure_ascii=False, separators=(",", ":"))
    print(f"{output}: {len(patches)} factory patches")


if __name__ == "__main__":
    main()
