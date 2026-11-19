#!/usr/bin/env bash
# Build the Prism WebCLAP bundle (eight analyzers) into build/prism/web/prism.wclap. Needs Emscripten (emsdk), node
# and the CLAP headers from fetch-deps.py.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/prism"
BUNDLE="$BUILD/web/prism.wclap"

if ! command -v em++ >/dev/null 2>&1; then
    for env in "${EMSDK:-}/emsdk_env.sh" "$HOME/emsdk/emsdk_env.sh" "$HOME/Development/emsdk/emsdk_env.sh"; do
        if [ -f "$env" ]; then source "$env" >/dev/null 2>&1; break; fi
    done
fi

python3 "$ROOT/scripts/fetch-deps.py"

rm -rf "$BUNDLE"
mkdir -p "$BUNDLE"
OUT="$BUNDLE" CLAP_INCLUDE="$ROOT/external/clap/include" PRISM="$ROOT/external/prism" UI="$BUILD/ui" \
    "$ROOT/ports/prism/build.sh"

# License and notices: GPL-3.0 (Prism's), no JUCE; dr_libs (public domain or MIT-0) and r8brain (MIT) in the reference
# analysis, React (MIT), zustand (MIT), the JetBrains Mono font (OFL-1.1) in the page
source "$ROOT/scripts/bundle-license.sh"
UI_MODULES="$BUILD/ui/node_modules"
BUNDLE_LICENSE="$ROOT/ports/prism/LICENSE" bundle_license "$BUNDLE" "$ROOT/ports/prism/NOTICES.md.in" \
    "$ROOT/external/prism/THIRD_PARTY_NOTICES.md" Prism-THIRD_PARTY_NOTICES.md \
    "$ROOT/external/prism/native/vendor/dr_libs/LICENSE" dr_libs-LICENSE.txt \
    "$ROOT/external/prism/native/vendor/r8brain/LICENSE" r8brain-LICENSE.txt \
    "$UI_MODULES/react/LICENSE" React-LICENSE.txt \
    "$UI_MODULES/zustand/LICENSE" zustand-LICENSE.txt \
    "$UI_MODULES/@fontsource/jetbrains-mono/LICENSE" JetBrainsMono-OFL.txt

python3 "$ROOT/scripts/pack-wclap.py" "$BUNDLE" "$BUILD/prism.wclap.tar.gz"
node "$ROOT/ports/prism/check.mjs" "$BUNDLE/module.wasm"
echo "Built $BUNDLE — try it on the site (cd site && npm run dev): http://localhost:8130/prism-spectrum?local"
