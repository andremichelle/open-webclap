# The ZL Audio ports' build, sourced by scripts/build-zl-*.sh after setting PORT (the port and external/ folder).
#
#   zl_build [cmake args]          fetches the deps, configures and builds build/$PORT
#   zl_bundle [file name]...       copies the page, bundles the licenses (the plugin's AGPL-3.0 or GPL-3.0 text,
#                                  Inter's OFL and Apache-2.0 for Material Symbols if it embeds them, DejaVu,
#                                  JUCE's, then the given files) and packs the archive
#
# ZL_WINDOW=0 before zl_bundle: a port that is its device panel alone (no editor, no ui/ui.wasm): the bundle's
# ui/ holds the panel's page and its controls only, and none of the editor's fonts and icons' licenses

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build/$PORT"
WEB="$BUILD/web"
BUNDLE="$WEB/$PORT.wclap"

if ! command -v emcmake >/dev/null 2>&1; then
    for env in "${EMSDK:-}/emsdk_env.sh" "$HOME/emsdk/emsdk_env.sh" "$HOME/Development/emsdk/emsdk_env.sh"; do
        if [ -f "$env" ]; then source "$env" >/dev/null 2>&1; break; fi
    done
fi

zl_build() {
    python3 "$ROOT/scripts/fetch-deps.py"
    emcmake cmake -S "$ROOT/ports/$PORT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release "$@"
    cmake --build "$BUILD" -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
}

zl_bundle() {
    # The bundle's page (ui/), the device panel's shared controls beside it (ui/opendaw-page)
    mkdir -p "$BUNDLE/ui/opendaw-page"
    if [ "${ZL_WINDOW:-1}" = 0 ]; then
        cp "$ROOT/ports/$PORT/ui/panel.html" "$BUNDLE/ui/"
    else
        cp "$ROOT/modules/juce_webclap/js/"*.js "$ROOT/ports/$PORT/ui/"* "$BUNDLE/ui/"
    fi
    cp "$ROOT/ports/zl-common/ui/"*.js "$BUNDLE/ui/"
    cp "$ROOT/modules/opendaw-page/panel.css" "$ROOT/modules/opendaw-page/panel.js" "$BUNDLE/ui/opendaw-page/"

    # License and notices (LICENSE, NOTICES.md, licenses/)
    # The plugin's own license by its title (the GPL-3.0 text names the Affero license further down), Inter's OFL
    # for the plugins that embed Inter, Apache-2.0 for those with Material Symbols icons
    local license inter=() fonts=()
    license="$(ls "$ROOT/external/$PORT"/LICENSE* | head -1)"
    if [ "${ZL_WINDOW:-1}" != 0 ]; then
        if ls "$ROOT/external/$PORT"/assets/font/Inter* >/dev/null 2>&1; then
            inter+=("$ROOT/licenses/OFL-1.1.txt" OFL-1.1.txt)
        fi
        if [ -d "$ROOT/external/$PORT/assets/icons" ]; then
            inter+=("$ROOT/licenses/Apache-2.0.txt" Apache-2.0.txt)
        fi
        fonts+=("$ROOT/external/fonts/DejaVu-LICENSE" DejaVu.txt)
    fi
    source "$ROOT/scripts/bundle-license.sh"
    bundle_license "$BUNDLE" "$ROOT/ports/$PORT/NOTICES.md.in" \
        "$license" "$(head -5 "$license" | grep -qi 'Affero' && echo AGPL-3.0.txt || echo GPL-3.0.txt)" \
        ${inter[@]+"${inter[@]}"} \
        ${fonts[@]+"${fonts[@]}"} \
        "$@" \
        "${JUCE_CORE_LICENSES[@]}" "${JUCE_GRAPHICS_LICENSES[@]}"

    # The archive hosts import (openDAW: Import WebCLAP...)
    python3 "$ROOT/scripts/pack-wclap.py" "$BUNDLE" "$BUILD/$PORT.wclap.tar.gz"
    echo "Built $BUNDLE — try it on the site: cd site && npm run dev, then http://localhost:8130/$PORT?local"
}
