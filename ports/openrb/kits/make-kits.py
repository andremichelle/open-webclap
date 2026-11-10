"""Builds OpenRB's factory kits: 16-bit 44.1 kHz mono WAVs, one per slot, from free samples plus a few synthesised
layers. Usage: make_kits.py <downloads dir> <reference aif dir or -> <out dir>

The reference AIFFs (ReBirth's own, never shipped) are only used, when given, to match each slot's loudness and to
print a similarity score; without them the stored gains below are used."""
import json, os, subprocess, sys, wave
import numpy as np
from scipy.signal import butter, sosfilt

SR = 44100
DL, REF, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
FS808, FL, JGB = "freesound-karman_lyne-tr08", "sf-fluidr3mono/kits/025_TR_808", "freesound-altemark-tr909jgb"

# slot -> source file (under DL) or a synth name; tune = semitones of pitch shift
P808 = {
    "BD": (f"{FS808}/519306_TR08_Kd01.WAV.mp3", 0), "CB": (f"{FS808}/519303_TR08_CowBell01.WAV.mp3", 0),
    "CH": (f"{FS808}/519308_TR08_HhClosed01.WAV.mp3", 0), "OH": (f"{FS808}/519307_TR08_HhOpen01.WAV.mp3", 0),
    "RS": (f"{FS808}/520172_TR08_RimShot01.WAV.mp3", 0), "CL": (f"{FS808}/519304_TR08_Clave01.WAV.mp3", 0),
    "CP": (f"{FL}/039-039_808_Clap.wav", 0),
    "HC": (f"{FL}/062-062_808_High_Conga.wav", "f0"), "MC": (f"{FL}/063-063_808_Mid_Conga.wav", "f0"),
    "LC": (f"{FL}/064-064_808_Low_Conga.wav", "f0"), "LT": (f"{FL}/041-041_808_Tom_1.wav", "f0"),
    "MT": (f"{FL}/043-043_808_Tom_2.wav", "f0"), "HT": (f"{FL}/045-045_808_Tom_3.wav", "f0"),
    "SD1": ("sd808:0", 0), "SD2": ("sd808:1", 0), "SD3": ("sd808:2", 0), "SD4": ("sd808:3", 0), "SD5": ("sd808:4", 0),
    "N1": ("n808:0", 0), "N2": ("n808:1", 0), "N3": ("n808:2", 0), "N4": ("n808:3", 0),
    "CY": ("cy808", 0), "MA": ("ma808", 0),
}
P909 = {
    "bd1": (f"{JGB}/26502_bd16.wav.mp3", 0), "bd2": (f"{JGB}/26508_bd22.wav.mp3", 0),
    "bd3": (f"{JGB}/26509_bd23.wav.mp3", 0), "bd4": (f"{JGB}/26513_bd27.wav.mp3", 0),
    "sd1": (f"{JGB}/26688_sn15.wav.mp3", "f0"), "sd2": (f"{JGB}/26688_sn15.wav.mp3", "f0"),
    "sd3": (f"{JGB}/26689_sn16.wav.mp3", "f0"), "sd4": (f"{JGB}/26690_sn17.wav.mp3", "f0"),
    "sd5": (f"{JGB}/26691_sn18.wav.mp3", "f0"),
    "sdn1": ("n909:0", 0), "sdn2": ("n909:1", 0), "sdn3": ("n909:2", 0),
    "lt": (f"{JGB}/26610_lt15.wav.mp3", 0), "mt": (f"{JGB}/26642_mt23.wav.mp3", 0),
    "ht": (f"{JGB}/26577_ht04.wav.mp3", 0), "rs": (f"{JGB}/26672_rs03.wav.mp3", 0),
    "hc": (f"{JGB}/26555_clp04.wav.mp3", 0), "chh": (f"{JGB}/26523_ch04.wav.mp3", 0),
    "ohh": (f"{JGB}/26644_oh02.wav.mp3", 0), "ccy": (f"{JGB}/26662_ride05.wav.mp3", 0),
    "rcy": (f"{JGB}/26663_ride06.wav.mp3", 0),
}


def read(path):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", str(SR), "-f", "f32le", "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype="<f4").astype(np.float64)


def decay_env(n, t40, attack=0.0005):
    t = np.arange(n) / SR
    env = np.exp(-t * np.log(100.0) / t40)
    a = int(attack * SR)
    if a > 0:
        env[:a] *= np.linspace(0, 1, a)
    return env


def env2(n, t20, t40, attack):  # -20 dB at t20, -40 dB at t40, a linear attack
    t = np.arange(n) / SR
    db = np.where(t < t20, -20 * t / t20, -20 - 20 * (t - t20) / max(t40 - t20, 1e-3))
    e = 10 ** (db / 20)
    a = int(attack * SR)
    if a > 0:
        e[:a] *= np.linspace(0, 1, a)
    return e


def noise(seed, n):
    return np.random.default_rng(seed).standard_normal(n)


def synth(name):
    kind, _, i = name.partition(":")
    i = int(i or 0)
    if kind == "sd808":  # the 808 snare's tone layer: two damped oscillators, the upper one rising with the tone
        t40 = [0.12, 0.115, 0.105, 0.09, 0.085][i]
        n = int(t40 * 1.6 * SR); t = np.arange(n) / SR
        upper = [0.0, 0.2, 0.45, 0.9, 1.2][i]
        x = np.sin(2 * np.pi * 180 * t) + upper * np.sin(2 * np.pi * 332 * t) * np.exp(-t * 18)
        return x * decay_env(n, t40)
    if kind == "n808":  # the 808 snare's noise layer (snappy)
        t40 = [0.13, 0.135, 0.13, 0.125][i]
        n = int(t40 * 1.6 * SR)
        hp = [1700, 1650, 1800, 1900][i]
        x = sosfilt(butter(2, hp, "highpass", fs=SR, output="sos"), noise(808 + i, n))
        x = sosfilt(butter(2, 13000, "lowpass", fs=SR, output="sos"), x)
        return x * decay_env(n, t40, 0.001)
    if kind == "n909":  # the 909 snare's noise layer
        t40 = [0.26, 0.255, 0.265][i]
        n = int(t40 * 1.6 * SR)
        x = sosfilt(butter(1, 3000, "highpass", fs=SR, output="sos"), noise(909 + i, n))
        x = sosfilt(butter(2, 9000, "lowpass", fs=SR, output="sos"), x)
        return x * env2(n, 0.22, t40, 0.001)
    if kind == "cy808":  # six square oscillators through two band-passes, the 808's metal
        n = int(2.4 * SR); t = np.arange(n) / SR
        sq = sum(np.sign(np.sin(2 * np.pi * f * t + k)) for k, f in enumerate([205.3, 304.4, 369.6, 522.7, 540.0, 800.0]))
        hi = sosfilt(butter(2, [8000, 12800], "bandpass", fs=SR, output="sos"), sq)
        lo = sosfilt(butter(2, [3000, 4200], "bandpass", fs=SR, output="sos"), sq)
        x = hi * env2(n, 0.25, 1.78, 0.0005) + 0.5 * lo * env2(n, 0.05, 0.3, 0.0005)
        return sosfilt(butter(2, 3000, "highpass", fs=SR, output="sos"), x)
    if kind == "ma808":  # a short burst of high noise
        n = int(0.06 * SR)
        x = sosfilt(butter(2, 9000, "highpass", fs=SR, output="sos"), noise(19, n))
        return x * env2(n, 0.02, 0.032, 0.002)
    raise ValueError(name)


def f0_of(x):  # the strongest partial 30..1500 Hz in 15..120 ms
    seg = x[int(0.015 * SR):int(0.12 * SR)]
    spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), 1 << 16))
    f = np.fft.rfftfreq(1 << 16, 1 / SR)
    m = (f > 30) & (f < 1500)
    return f[m][np.argmax(spec[m])]


def shift(x, ratio):  # resample: ratio > 1 raises the pitch (and shortens)
    pos = np.arange(0, len(x) - 1, ratio)
    return np.interp(pos, np.arange(len(x)), x)


def peak_rms(x):
    n = len(x) // 441
    return np.sqrt(np.mean(x[: n * 441].reshape(n, 441) ** 2, axis=1)).max()


def trim(x):
    a = np.abs(x)
    start = max(0, int(np.argmax(a > a.max() * 0.01)) - 22)   # 0.5 ms before -40 dB of peak
    x = x[start:].copy()
    x[:22] *= np.linspace(0, 1, 22) ** 2 if start > 0 else 1
    q = np.round(x * 32767)
    loud = np.nonzero(np.abs(q) > 6)[0]                       # the tail ends below 6 / 32767 (-75 dBFS)
    end = int(loud[-1]) + 1 if len(loud) else 1
    x = x[:end]
    f = min(len(x), 256)
    x[-f:] *= np.linspace(1, 0, f)
    return x


def build(machine, plan, names, stored):
    """Writes the slots. With the references, each slot's pitch ratio and gain are measured and stored in kit.json;
    without them (REF "-") they are read from it."""
    if REF != "-":
        sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "drum-match", "tools"))
        from features import features, distance
        from audio_io import read_aiff_ch0
    os.makedirs(os.path.join(OUT, machine), exist_ok=True)
    for slot in names:
        src, tune = plan[slot]
        x = synth(src) if not src.startswith(("freesound", "sf-")) else read(os.path.join(DL, src))
        key = f"{machine}/{slot}"
        if REF != "-":
            ref = read_aiff_ch0(os.path.join(REF, f"TR{machine}{slot}.aif"))[0]
            ratio = f0_of(ref) / f0_of(x) if tune == "f0" else 1.0
            y = shift(x, ratio) if ratio != 1.0 else x
            gain = peak_rms(ref) / peak_rms(y)
            stored[key] = {"source": src, "ratio": round(float(ratio), 6), "gain": round(float(gain), 6)}
        ratio, gain = stored[key]["ratio"], stored[key]["gain"]
        if ratio != 1.0:
            x = shift(x, ratio)
        x = x * gain
        if np.abs(x).max() > 0.999:
            x = x * 0.999 / np.abs(x).max()
        x = trim(x)
        pcm = np.round(x * 32767).astype("<i2")
        with wave.open(os.path.join(OUT, machine, f"{slot}.wav"), "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes(pcm.tobytes())
        if REF != "-":
            pitched = slot.upper().rstrip("12345") in ("BD", "HC", "MC", "LC", "LT", "MT", "HT", "CB")
            tot, _ = distance(features(pcm / 32768.0), features(ref), pitched)
            stored[key]["score"] = round(100 * float(np.exp(-1.5 * tot)), 1)
        print(f"{machine} {slot:5s} {len(pcm):7d} frames  {stored[key].get('score', '')}  {src}")


N808 = ["BD", "SD1", "SD2", "SD3", "SD4", "SD5", "N1", "N2", "N3", "N4", "CB", "CH", "CL", "CP", "CY",
        "HC", "HT", "LC", "LT", "MA", "MC", "MT", "OH", "RS"]
N909 = ["bd1", "bd2", "bd3", "bd4", "sd1", "sd2", "sd3", "sd4", "sd5", "sdn1", "sdn2", "sdn3", "lt",
        "mt", "ht", "rs", "hc", "chh", "ohh", "ccy", "rcy"]
KIT = os.path.join(OUT, "kit.json")
stored = {} if REF != "-" else json.load(open(KIT))
build("808", P808, N808, stored)
build("909", P909, N909, stored)
if REF != "-":
    json.dump(stored, open(KIT, "w"), indent=1)
