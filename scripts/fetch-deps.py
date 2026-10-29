#!/usr/bin/env python3
"""Fetch the pinned third-party sources into external/ and apply our patches.

Files are fetched one by one (GitHub tree API + raw.githubusercontent.com) instead of git clones or tarballs:
it only downloads what the build needs and survives flaky connections, since every file is retried on its own.

Usage: scripts/fetch-deps.py [--force]
"""

import io
import json
import os
import subprocess
import sys
import tarfile
import time
import urllib.parse
import urllib.request
import concurrent.futures as cf

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXTERNAL = os.path.join(ROOT, "external")

JUCE_MODULES = ["juce_core", "juce_events", "juce_data_structures", "juce_graphics", "juce_gui_basics",
                "juce_gui_extra", "juce_audio_basics", "juce_audio_processors", "juce_audio_processors_headless",
                "juce_audio_utils", "juce_audio_devices", "juce_audio_formats", "juce_dsp"]
JUCE_SKIP = ["VST3_SDK", "LV2_SDK", "/lilv/", "/serd/", "/sord/", "/sratom/", "/lv2/", "/zix/", "pslextensions",
             "/java/", "javaopt", "oboe", "oggvorbis"]


def juce_filter(path):
    parts = path.split("/")
    if path.startswith("modules/") and len(parts) == 2:
        return True
    return (path.startswith("modules/") and len(parts) > 2 and parts[1] in JUCE_MODULES
            and not any(s in path for s in JUCE_SKIP))


def obxf_filter(path):
    if path.startswith("assets/installer"):
        return "Themes/Default/" in path and "@4x" not in path or "/Patches/" in path
    return path.startswith(("src/", "assets/", "cmake/", "libs/CMakeLists")) or "/" not in path


# (directory, repo, commit, filter). Commits are the submodule pins of OB-Xf main at the time of writing.
DEPS = [
    ("OB-Xf", "surge-synthesizer/OB-Xf", "b08ffb6ab6cfa0f66cb057e855149ab640de0f78", obxf_filter),
    ("JUCE", "juce-framework/JUCE", "a8c7c714c7564b9248749e1e8cefcc5ef2b6f0cc", juce_filter),
    ("sst-cpputils", "surge-synthesizer/sst-cpputils", "ca88c2e02cef7142856bc8cf2ad4f0b298ed406a",
     lambda p: p.startswith("include/")),
    ("sst-basic-blocks", "surge-synthesizer/sst-basic-blocks", "1f1825b083e2c113c63fc5226f1fdd394982ac13",
     lambda p: p.startswith("include/")),
    ("sst-plugininfra", "surge-synthesizer/sst-plugininfra", "f6deb258ca2f99e48d8ddc3e5bdf4f245e15052b",
     lambda p: p.startswith(("include/", "src/", "libs/filesystem/", "libs/strnatcmp/"))),
    ("fmt", "fmtlib/fmt", "407c905e45ad75fc29bf0f9bb7c5c2fd3475976f", lambda p: p.startswith(("include/", "src/"))),
    ("simde", "simd-everywhere/simde", "71fd833d9666141edcd1d3c109a80e228303d8d7", lambda p: p.startswith("simde/")),
    ("ghc-filesystem", "gulrak/filesystem", "614bbe87b80435d87ab8791564370e0c1d13627d",
     lambda p: p == "include/ghc/filesystem.hpp"),
    ("clap", "free-audio/clap", "a47f6badb49d948fd009998f28309cdab78979c9", lambda p: p.startswith("include/")),
    ("MTS-ESP", "oddsound/MTS-ESP", "ce3f30e812744d8319313d80b92781bc3bcf4e18", lambda p: p.startswith("Client/")),
]


def six_sines_filter(path):
    return (path.startswith(("src/", "resources/factory_patches/", "resources/factory_themes/", "resources/fonts/",
                             "resources/icon/", "cmake/CmakeRC.cmake"))
            or path in ("doc/ack.md", "LICENSE.md", "resources/LICENSE_GPL3"))


def include_only(path):
    return path.startswith("include/")


# Six Sines and its submodules at the pins of six-sines main at the time of writing, laid out like its libs/ so the
# port reads like the upstream build. JUCE is the shared external/JUCE (newer than Six Sines' 8.0.10 pin).
SIX = "six-sines/libs/"
DEPS += [
    ("six-sines", "baconpaul/six-sines", "03f3b9af04511705148bca654a57f68742b5fb08", six_sines_filter),
    (SIX + "sst/sst-jucegui", "surge-synthesizer/sst-jucegui", "3aeed6ea9508e7aa2b12cf33c1f6252be7ec22b6",
     lambda p: p.startswith(("include/", "src/", "res/glyphs/"))),
    (SIX + "sst/sst-basic-blocks", "surge-synthesizer/sst-basic-blocks", "d9c4ce25a739afa8b8e850a8b973a5a912c34ea7",
     include_only),
    (SIX + "sst/sst-cpputils", "surge-synthesizer/sst-cpputils", "df02974f3d1a0da9f1caeb0cd65c944d068613a8",
     include_only),
    (SIX + "sst/sst-filters", "surge-synthesizer/sst-filters", "abef77ff1c32fe65260d84c3f8a3e637474213fc",
     include_only),
    (SIX + "sst/sst-voicemanager", "surge-synthesizer/sst-voicemanager", "c7dec29113ff9391af2029246208c46b85693164",
     include_only),
    (SIX + "sst/sst-plugininfra", "surge-synthesizer/sst-plugininfra", "783b28d35d47176badd3ce34453ce27b99591f3d",
     lambda p: p.startswith(("include/", "src/", "libs/tinyxml/include/", "libs/tinyxml/src/", "libs/strnatcmp/"))),
    (SIX + "sst/sst-plugininfra/libs/miniz", "richgel999/miniz", "16413c213de38e703d883006193734e8b1178d5d",
     lambda p: "/" not in p and p.endswith((".c", ".h", ".txt", ".md"))),
    (SIX + "sst/sst-clap-helpers", "surge-synthesizer/sst-clap-helpers", "f17567a8166a50bfb50ddc9acca7e77c72e6096f",
     lambda p: p.startswith(("include/", "src/"))),
    (SIX + "clap-libs/clap", "free-audio/clap", "29ffcc273be7c7c651f6c9953b99e69700e2387a", include_only),
    (SIX + "clap-libs/clap-helpers", "free-audio/clap-helpers", "a61bcdf0ecc2c8db1e80bfe8bf9cb7e8d9fd2bbc",
     include_only),
    (SIX + "fmt", "fmtlib/fmt", "407c905e45ad75fc29bf0f9bb7c5c2fd3475976f", lambda p: p.startswith(("include/", "src/"))),
    (SIX + "libsamplerate", "libsndfile/libsamplerate", "15c392d47e71b9395a759544b3818a1235fe1a1d",
     lambda p: p.startswith(("src/", "include/"))),
    (SIX + "pffft", "surge-synthesizer/pffft", "d027f2a290ca0047d95513c2a20570d13557e381",
     lambda p: "/" not in p and p.endswith((".c", ".h", ".hpp", ".cpp", ".txt", ".md"))),
    (SIX + "simde", "simd-everywhere/simde", "60a3a24462d5c074f669ed8b39c85f5b1128108f", lambda p: p.startswith("simde/")),
    (SIX + "MTS-ESP", "ODDSound/MTS-ESP", "f214739b8832e7f297cb9970d0c0efbf783f1462", lambda p: p.startswith("Client/")),
]

# RipplerX and its MTS-ESP submodule, at the pins of ripplerx master at the time of writing. JUCE is the shared one.
DEPS += [
    ("ripplerx", "tiagolr/ripplerx", "11683a6bb1c4eac526c6d44af97abfd83cd6796d",
     lambda p: p.startswith(("src/", "resources/")) or p in ("LICENSE", "README.md")),
    ("ripplerx/libs/MTS-ESP", "ODDSound/MTS-ESP", "2d7c013ebf4a076c35811e62293e8f819d053a91",
     lambda p: p.startswith("Client/")),
]


def odin2_filter(path):
    if path.startswith("assets/"):
        return not path.endswith((".xcf", ".sh"))
    return path.startswith("Source/") or path in ("CMakeLists.txt", "LICENSE", "README.md")


# Odin 2 and the submodules it builds with, at the pins of odin2 master at the time of writing. JUCE is the shared
# one (Odin pins a 2022 JUCE), JUCELV2 and clap-juce-extensions are not needed.
DEPS += [
    ("odin2", "TheWaveWarden/odin2", "265e9e227581357efb87ed7217381f8a40c5e2bf", odin2_filter),
    ("odin2/libs/tuning-library", "surge-synthesizer/tuning-library", "8eb7232ac24ac2f97a052063161c0c4f5fd763e3",
     lambda p: p.startswith("include/") or p == "LICENSE.md"),
    ("odin2/libs/json", "nlohmann/json", "960b763ecd144f156d05ec61f577b04107290137",
     lambda p: p.startswith("include/") or p == "LICENSE.MIT"),
]

def surge_filter(path):
    if path.startswith("resources/data/"):
        return "_3rdparty/" not in path
    if path.startswith("libs/sqlite-3.23.3/"):
        return path.endswith(("sqlite3.c", "sqlite3.h", "sqlite3ext.h"))
    return (path.startswith(("src/common/", "src/surge-xt/", "src/platform/", "src/lua/", "libs/airwindows/",
                             "libs/binn/", "libs/eurorack/", "resources/surge-shared/", "resources/fonts/",
                             "resources/classic-skin-svgs/"))
            or path in ("src/cmake/CmakeRC.cmake", "LICENSE", "AUTHORS.md", "README.md",
                        "resources/surge-xt/memory-skin.xml", "resources/surge-xt/wtfile_icon.svg",
                        "resources/surge-xt/wtscript_icon.svg"))


# Surge XT and the submodules it builds with, at the pins of surge main at the time of writing. JUCE (Surge pins the
# same commit) and simde are the shared ones. LuaJIT, MTS-ESP, clap-juce-extensions are not needed (no Lua yet, the
# kit wraps the processor).
SURGE = "surge/libs/"
DEPS += [
    ("surge", "surge-synthesizer/surge", "4f425b04b7c08cffa1f45ec3851607d269d47444", surge_filter),
    (SURGE + "eurorack/eurorack", "surge-synthesizer/eurorack", "888298b0e3ec5daabf96c5ab912ffc42825a2b4c",
     lambda p: p.endswith((".cc", ".h", ".hpp")) or p == "LICENSE"),
    (SURGE + "PEGTL", "taocpp/PEGTL", "64af78c6a7959cd5753ad165ec9f65591aa96f2d",
     lambda p: p.startswith("include/") or p == "LICENSE"),
    (SURGE + "fmt", "fmtlib/fmt", "e424e3f2e607da02742f73db84873b8084fc714c",
     lambda p: p.startswith(("include/", "src/")) or p == "LICENSE"),
    (SURGE + "tuning-library", "surge-synthesizer/tuning-library", "5a18353a0444caa068b648383cae37d156b97e1c",
     lambda p: p.startswith("include/") or p in ("LICENSE.md", "README.md")),
    (SURGE + "pffft", "surge-synthesizer/pffft", "d027f2a290ca0047d95513c2a20570d13557e381",
     lambda p: "/" not in p and p.endswith((".c", ".h", ".hpp", ".cpp", ".txt", ".md"))),
    (SURGE + "r8brain-free-src", "avaneev/r8brain-free-src", "b3f0753d90b3325b8ea8a08fd699e8e6db26e5b5",
     lambda p: p.endswith((".cpp", ".h", ".c", ".inc")) and not p.startswith(("bench/", "other/", "DLL/"))
     or p == "LICENSE"),
    (SURGE + "zstd", "facebook/zstd", "92505deb29bc17fe014ed918e67f15a84d70b230",
     lambda p: p.startswith(("lib/common/", "lib/compress/", "lib/decompress/"))
     or p in ("lib/zstd.h", "lib/zstd_errors.h", "lib/zdict.h", "LICENSE")),
    (SURGE + "sst/sst-basic-blocks", "surge-synthesizer/sst-basic-blocks", "7209f14567b37a0db6986c7cb1680089e97c8516",
     include_only),
    (SURGE + "sst/sst-cpputils", "surge-synthesizer/sst-cpputils", "52c4b749c0e155077ee1df79fee5d237bef7a1a8",
     include_only),
    (SURGE + "sst/sst-effects", "surge-synthesizer/sst-effects", "5f2bd1a8fdea77f9f2c8acad3ed71e66fd7fa41d",
     include_only),
    (SURGE + "sst/sst-filters", "surge-synthesizer/sst-filters", "6bcf08bb8ecd177b161281e8147f30b10f0b5db7",
     lambda p: p.startswith(("include/", "include-extras/"))),
    (SURGE + "sst/sst-waveshapers", "surge-synthesizer/sst-waveshapers", "dd12f31a5a9016c9895e52d1a00eee0e1eebe6ce",
     include_only),
    (SURGE + "sst/sst-plugininfra", "surge-synthesizer/sst-plugininfra", "ebdaf5e936361994a30b0d51d2ff6cb750232d4b",
     lambda p: p.startswith(("include/", "src/", "libs/tinyxml/include/", "libs/tinyxml/src/", "libs/strnatcmp/",
                             "libs/filesystem/"))),
]

# coopiano (Peter Cooper) at main at the time of writing: header-only C++17, no submodules
DEPS += [
    ("coopiano", "peterc/coopiano", "bba9e06713552fadf33d93490526ae9c2ab66112",
     lambda p: p.endswith((".hpp", ".cpp", ".ini")) or p in ("LICENSE", "README.md", "Makefile")),
]

# Only the three small HRTF sets are hosted, the KU100 sets are 20 and 37 MB
OSD_HRTF = ("HRTF/mit_kemar_large_pinna.sofa", "HRTF/cipic_subject_003.sofa", "HRTF/hutubs_pp2.sofa")

# OpenSpatialDelay (Spatial Media Lab) at main at the time of writing, and libmysofa at the v1.3.2 its CMake fetches.
# JUCE is the shared one.
DEPS += [
    ("open-spatial-delay", "Spatial-Media-Lab/OpenSpatialDelay", "14947a0ffa9e26087e546a141b437a93d5c4ae45",
     lambda p: p.startswith(("Source/", "fonts/")) or p in OSD_HRTF or p in ("LICENSE", "README.md")),
    ("libmysofa", "hoene/libmysofa", "33974839677da7044ef5a70be7ad5550735aae6e",
     lambda p: p.startswith(("src/hrtf/", "src/hdf/", "src/resampler/")) or p == "LICENSE"),
]


def zl_filter(path):
    return (path.startswith(("source/", "Source/", "assets/", "resources/"))
            or path in ("LICENSE", "LICENSE.md", "README.md", "VERSION", "CMakeLists.txt"))


# The ZL Audio plugins (zsliu98) at main at the time of writing, each with the submodules it builds with at its pins:
# Google Highway, zldsp_fft (on Highway), nlopt (ZLEqualizer's EQ match), KFR (ZLSplitter) and friz (ZLWarm's
# animations). JUCE is the shared one (ZL pins a fork with four small changes, see patches/zl-*.patch).
ZL = [
    ("zl-equalizer", "ZLEqualizer", "3ce598ee065b3d3847121e76e196feda7ed8f56d"),
    ("zl-compressor", "ZLCompressor", "007478995b3bfde5bc756f404721ba37617dce65"),
    ("zl-splitter", "ZLSplitter", "a8f31f6251933ff2e3366dde33939a68be5c0e71"),
    ("zl-spectrum-equalizer", "ZLSpectrumEqualizer", "10388f0cb594c6a322f8652cd718aed28c2e2c39"),
    ("zl-limiter", "ZLLimiter", "cb52b21f0d3c7c075acf3cbb13fb4eccedcfa184"),
    ("zl-inflator", "ZLInflator", "b71bf485eb84e37b61f8f15e7e04ce190d34230d"),
    ("zl-warm", "ZLWarm", "48093f38c079551924679c5b86bd02d348885f3a"),
    ("zl-lmatch", "ZLLMatch", "0a567f885bb11f479d220afbb1179c028494c0a2"),
]
DEPS += [(directory, "ZL-Audio/" + repo, commit, zl_filter) for directory, repo, commit in ZL]
DEPS += [
    ("zl-highway", "ZL-Audio/highway", "2607d3b5b0113992fe84d3848859eae13b3b52c1",
     lambda p: p.startswith("hwy/") and "/tests/" not in p and not p.endswith("_test.cc") or p == "LICENSE"),
    ("zl-nlopt", "ZL-Audio/nlopt", "ab0fd1ca07d11d6c2f52c4af0423ec02505352ea",
     lambda p: p.startswith("src/") and not p.startswith(("src/octave/", "src/swig/")) or p in (
         "COPYING", "COPYRIGHT", "nlopt_config.h.in", "nlopt.pc.in", "CMakeLists.txt") or p.startswith("cmake/")),
    ("zl-kfr", "ZL-Audio/kfr", "9a35250946f33c0ce61a05fba32a88223211fdae",
     lambda p: p.startswith(("include/", "src/")) or p == "LICENSE.txt"),
    ("zl-friz", "ZL-Audio/animator", "9c3f6e04e74f67f1fdf84574e163dd05bb1e5480",
     lambda p: p.startswith("Source/") or p == "LICENSE"),
]
# zldsp_fft where the plugins include it from (relative to their source/)
DEPS += [(directory + "/zldsp_fft", "ZL-Audio/zldsp_fft", "dfc50e6c39680d9ee04fbb60fc6c935552630045",
          lambda p: p.startswith("src/") or p == "LICENSE.md")
         for directory in ("zl-equalizer", "zl-compressor", "zl-spectrum-equalizer")]

FONT_PACKAGE ="https://registry.npmjs.org/dejavu-fonts-ttf/-/dejavu-fonts-ttf-2.37.3.tgz"
FONT_FILES = ["package/ttf/DejaVuSans.ttf", "package/ttf/DejaVuSans-Bold.ttf", "package/LICENSE"]


def curl(url, out=None, retries=8):
    for attempt in range(retries):
        args = ["curl", "-sSfL", "--max-time", "300", url]
        if out:
            args += ["-o", out]
        result = subprocess.run(args, capture_output=True)
        if result.returncode == 0:
            return result.stdout
        time.sleep(1 + attempt)
    raise RuntimeError(f"download failed: {url}")


def fetch_repo(directory, repo, commit, keep, force):
    dest = os.path.join(EXTERNAL, directory)
    stamp = os.path.join(dest, ".fetched")
    if not force and os.path.exists(stamp) and open(stamp).read().strip() == commit:
        print(f"{directory}: up to date")
        return
    tree = json.loads(curl(f"https://api.github.com/repos/{repo}/git/trees/{commit}?recursive=1"))
    if tree.get("truncated"):
        raise RuntimeError(f"{repo}: tree listing truncated")
    files = [e["path"] for e in tree["tree"] if e["type"] == "blob" and keep(e["path"])]

    def get(path):
        out = os.path.join(dest, path)
        if os.path.exists(out) and os.path.getsize(out) > 0 and not force:
            return
        os.makedirs(os.path.dirname(out), exist_ok=True)
        curl(f"https://raw.githubusercontent.com/{repo}/{commit}/" + urllib.parse.quote(path), out)
        with open(out, "rb") as file:
            lfs = file.read(40).startswith(b"version https://git-lfs.github.com/spec/")
        if lfs:
            curl(f"https://media.githubusercontent.com/media/{repo}/{commit}/" + urllib.parse.quote(path), out)

    with cf.ThreadPoolExecutor(16) as pool:
        list(pool.map(get, files))
    open(stamp, "w").write(commit + "\n")
    print(f"{directory}: {len(files)} files")


def fetch_fonts():
    dest = os.path.join(EXTERNAL, "fonts")
    if all(os.path.exists(os.path.join(dest, n)) for n in ("DejaVuSans.ttf", "DejaVuSans-Bold.ttf", "DejaVu-LICENSE")):
        print("fonts: up to date")
        return
    os.makedirs(dest, exist_ok=True)
    data = urllib.request.urlopen(FONT_PACKAGE).read()
    with tarfile.open(fileobj=io.BytesIO(data)) as tar:
        for name in FONT_FILES:
            target = os.path.join(dest, os.path.basename(name) if not name.endswith("LICENSE") else "DejaVu-LICENSE")
            open(target, "wb").write(tar.extractfile(name).read())
    print("fonts: DejaVu Sans")


# patches/<prefix>-*.patch apply to external/<directory>
PATCH_TARGETS = [("juce-", "JUCE"), ("six-sines-", "six-sines"), ("ripplerx-", "ripplerx"), ("odin2-", "odin2"),
                 ("surge-", "surge"), ("coopiano-", "coopiano"), ("open-spatial-delay-", "open-spatial-delay"),
                 ("libmysofa-", "libmysofa")] + [(d + "-", d) for d, _, _ in ZL]


def apply_patches():
    for prefix, directory in PATCH_TARGETS:
        target = os.path.join(EXTERNAL, directory)
        for name in sorted(p for p in os.listdir(os.path.join(ROOT, "patches")) if p.startswith(prefix)):
            path = os.path.join(ROOT, "patches", name)
            dry = lambda *extra: subprocess.run(["patch", "-p1", "-s", "-f", "--dry-run", *extra, "-i", path],
                                                cwd=target, capture_output=True).returncode == 0
            if dry("-R"):
                print(f"{name}: already applied")
            elif dry():
                subprocess.run(["patch", "-p1", "-s", "-i", path], cwd=target, check=True)
                print(f"{name}: applied")
            else:
                raise RuntimeError(f"{name} does not apply to external/{directory} (modified by hand?)")


def main():
    force = "--force" in sys.argv
    os.makedirs(EXTERNAL, exist_ok=True)
    for directory, repo, commit, keep in DEPS:
        fetch_repo(directory, repo, commit, keep, force)
    fetch_fonts()
    apply_patches()


if __name__ == "__main__":
    main()
