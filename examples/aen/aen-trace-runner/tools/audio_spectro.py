#!/usr/bin/env python3
"""Spectrogram PNG (+ level summary) for a 16-bit PCM clip -- a quick visual
check of synth previews and of the PDM-mic loopback capture.

  audio_spectro.py clip.wav [...]              -> clip.png beside each input
  audio_spectro.py --capture dump.bin [-o out] -> verdict + per-effect comparison against the
                                                  PC render of the same script (out_NN_<window>_cap.wav,
                                                  out.png), from a TEST capture read over SWD

Needs numpy and a host cc. The capture format ('TRC2', 512-byte header, mono
S16 at 48 kHz, per-window records) is documented in sound/src/main.c. With
--capture it applies sound/src/snd_verdict.h's rule (exit 1 on FAIL), renders
the same script on the PC with the image's synth build, and per window prints:
  excess  captured energy the render x speaker response (estimated from the
          music window) does not explain -- distortion, aliasing, buzz; near
          0 dB is bad, below -20 dB is clean
  LSD     energy-weighted log-spectral distance to that prediction
  HF>6k   captured vs predicted energy above 6 kHz
  clip%   mic samples at full scale; flat = repeated large samples
  refpeak the render's own peak (a synth-side overload shows here)
  tone    THD of the reference tones (level-dependent chain distortion)
  faults  each amp's latched faults over the window (limiter, brown-out, ...)
and ranks the effects. --selftest checks the verdict (all-zero capture,
write storm) and that a deliberately clipped effect ranks worst.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import wave
import zlib

import numpy as np

CAP_MAGIC = 0x50435254  # 'TRCP'


def png(path, rgb):
    h, w, _ = rgb.shape
    raw = b"".join(b"\x00" + rgb[y].tobytes() for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def spectro(x, rate, path, nfft=512, height=256):
    x = x.astype(np.float64) / 32768.0
    hop = max(1, len(x) // 1200) if len(x) > 1200 * 128 else 128
    win = np.hanning(nfft)
    frames = [x[i:i + nfft] * win for i in range(0, max(1, len(x) - nfft), hop)]
    if not frames:
        frames = [np.pad(x, (0, nfft - len(x))) * win]
    s = np.abs(np.fft.rfft(np.array(frames), axis=1)) + 1e-9
    db = 20 * np.log10(s / (nfft / 4))
    img = np.clip((db + 90) / 90, 0, 1).T[::-1]  # low frequencies at the bottom
    idx = np.linspace(0, img.shape[0] - 1, height).astype(int)
    img = img[idx]
    # magma-ish ramp: black -> purple -> orange -> yellow
    r = np.clip(img * 2.0, 0, 1)
    g = np.clip(img * 2.0 - 0.8, 0, 1)
    b = np.clip(np.sin(img * np.pi) * 0.8, 0, 1)
    rgb = (np.stack([r, g, b], axis=2) * 255).astype(np.uint8)
    png(path, rgb)
    peak = np.max(np.abs(x)) if len(x) else 0
    rms = np.sqrt(np.mean(x * x)) if len(x) else 0
    print("%s  %.2f s @ %d Hz  peak %.1f dBFS  rms %.1f dBFS  (y: 0..%d Hz)" % (
        path, len(x) / rate, rate, 20 * np.log10(peak + 1e-9), 20 * np.log10(rms + 1e-9), rate // 2))


MAX_FAILS, LIVE_RMS, TONE_FLOOR, MUSIC_FLOOR = 2, 0.5, 25.0, 5.0  # == snd_verdict.h
CAP2_MAGIC, HDR_BYTES = 0x32435254, 512  # 'TRC2', sound/src/main.c
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FAULT_BITS = {  # include/alp/chips/tas2563.h TAS2563_FAULT_* (INT_LTCH0..3)
    0: "overtemp", 1: "overcurrent", 2: "tdm_clock", 3: "limiter_active", 4: "vbat<inflection",
    5: "limiter_max_attn", 6: "limiter_inf_hold", 7: "limiter_mute", 8: "vbat_brownout",
    18: "boost_ov_clamp", 21: "vbat_por", 22: "boost_clock"}


def faults_str(f):
    names = [n for b, n in FAULT_BITS.items() if f >> b & 1]
    other = f & ~sum(1 << b for b in FAULT_BITS)
    return ",".join(names + (["0x%x" % other] if other else [])) or "-"


def parse(d):
    h = struct.unpack_from("<128I", d)
    if h[0] != CAP2_MAGIC:
        return None
    cap = {"rate": h[1], "frames": h[3], "wfail": h[5], "mfail": h[6], "restarts": h[7], "played": h[8],
           "want": h[9], "synth_rate": h[10], "ver": (h[11] & 0xFF) if h[11] & 0x100 else (2 if h[11] else 1), "volume": h[12], "overrides": h[13], "win": []}
    pcm = np.frombuffer(d[HDR_BYTES:HDR_BYTES + h[3] * 2], dtype="<i2").astype(np.float64)
    base = 0
    for w in range(h[4]):
        r = h[16 + 5 * w:21 + 5 * w]
        cap["win"].append({"type": r[0] & 0xFF, "kind": r[0] >> 8 & 0xFF, "param": r[0] >> 16 & 0xFF,
                           "hz": (r[0] >> 24) * 10,
                           "frames": r[1], "f0": r[2], "f1": r[3], "cyc": r[4], "pcm": pcm[base:base + r[1]]})
        base += r[1]
    return cap


def rms_bin(x, rate, hz):
    if len(x) == 0:
        return 0.0, 0.0
    x = x - x.mean()
    t = np.arange(len(x)) / rate
    return float(np.sqrt(np.mean(x * x))), float(abs(np.dot(x, np.exp(-2j * np.pi * hz * t))) ** 2 / len(x) ** 2)


def verdict(st):
    """st: rms/bin per window 0 (silence), 1 (1 kHz tone), 2 (music) + counters; == snd_verdict.h."""
    if st["want"] == 0:
        return "run not finished"
    if st["wfail"] > MAX_FAILS:
        return "I2S3 writes failed (%d)" % st["wfail"]
    if st["mfail"] > MAX_FAILS:
        return "PDM mic reads failed (%d)" % st["mfail"]
    if st["frames"] != st["want_frames"]:
        return "capture incomplete"
    if st["played"] > st["want"] + st["want"] // 10:
        return "playback slower than real time (%d ms for %d)" % (st["played"], st["want"])
    (r0, b0), (r1, b1), (r2, _) = st["w"]
    if not r0 >= LIVE_RMS:
        return "no live mic (silence window digitally zero)"
    if not (b1 >= TONE_FLOOR and b1 >= 100 * b0 and 2 * b1 >= 0.1 * r1 * r1):
        return "1 kHz tone not heard"
    if not (r2 >= MUSIC_FLOOR and r2 >= 2 * r0):
        return "music not heard"
    return None


def cap_stats(cap):
    w = cap["win"]
    # == sound/src/main.c: the script's blocks x 768, i.e. real-time ms / 16
    # minus the 4 pre-roll blocks -- NOT the capture's own window sizes, or
    # "capture incomplete" could never fire.
    want_frames = (cap["want"] // 16 - 4) * 768 if cap["want"] else 0
    return {"want": cap["want"], "wfail": cap["wfail"], "mfail": cap["mfail"], "frames": cap["frames"],
            "want_frames": want_frames, "played": cap["played"],
            "w": [rms_bin(w[first(w, t)]["pcm"], cap["rate"], 1000.0) for t in (0, 1, 2)]}


def first(win, t):
    """first window of type t: 0 silence, 1 tone (1 kHz -12 dBFS), 2 music"""
    return next(i for i, x in enumerate(win) if x["type"] == t)


def psd(x, rate, res=31.25):
    n = int(rate / res)
    if len(x) < n:
        x = np.pad(x, (0, n - len(x)))
    win = np.hanning(n)
    segs = [x[i:i + n] * win for i in range(0, len(x) - n + 1, n // 2)]
    p = np.mean(np.abs(np.fft.rfft(np.array(segs), axis=1)) ** 2, axis=0) / (win ** 2).sum()
    return p, np.fft.rfftfreq(n, 1.0 / rate)


def reference(cap, stem):
    """Render the image's script on the PC with the same synth build."""
    exe = stem + "_ref_render"
    ver = cap["ver"]
    cmd = ["cc", "-std=c11", "-O2", "-I" + os.path.join(REPO, "src"), "-DTR_AUDIO_V2=%d" % (ver >= 2),
           "-DTR_AUDIO_V3=%d" % (ver >= 3),
           "-DTR_AUDIO_RATE=%du" % cap["synth_rate"], "-o", exe, os.path.join(REPO, "tools/audio_preview.c"),
           os.path.join(REPO, "src/audio/tr_audio.c")]
    subprocess.run(cmd, check=True)
    subprocess.run([exe, "--script", stem + "_ref"], check=True, stdout=subprocess.DEVNULL)
    x, rate = read_wav(stem + "_ref.wav")
    names, clips, keys, base = [], [], [], 0
    for line in open(stem + "_ref.txt"):
        name, n, typ, kind, param, hz = line.split()
        names.append(name)
        clips.append(x[base:base + int(n)].astype(np.float64))
        keys.append((int(typ), int(kind), int(param), int(hz)))
        base += int(n)
    if "win" in cap:  # align to the capture's windows (older images ran fewer tones)
        sel, j = [], 0
        for w in cap["win"]:
            while j < len(keys) and not (keys[j][:3] == (w["type"], w["kind"], w["param"]) and
                                         (not w.get("hz") or keys[j][3] == w["hz"])):
                j += 1
            if j == len(keys):
                sys.exit("capture windows do not match this script (different image revision?)")
            sel.append(j)
            j += 1
        names, clips, keys = [names[k] for k in sel], [clips[k] for k in sel], [keys[k] for k in sel]
    return names, clips, rate, keys


def compare(cap, stem):
    names, refs, rrate, _ = reference(cap, stem)
    crate = cap["rate"]
    band_hi = min(7500.0, rrate / 2 - 250)
    P = [psd(w["pcm"], crate) for w in cap["win"]]
    R = [psd(r, rrate) for r in refs]
    f = R[0][1]
    nb = len(f)
    cp = [p[:nb] for p, _ in P]  # same 31.25 Hz grid, capture truncated to the reference band
    band = (f >= 150) & (f <= band_hi)
    noise = cp[0]
    mi = names.index("music")
    H = np.maximum(cp[mi] - noise, 1e-12) / np.maximum(R[mi][0], 1e-12)
    Hs = np.array([np.median(H[max(0, i - 6):i + 7]) for i in range(nb)])  # smoothed speaker+mic response
    rows = []
    for i, name in enumerate(names):
        w, x, ref = cap["win"][i], cap["win"][i]["pcm"], refs[i]
        pred = R[i][0] * Hs + noise
        c = cp[i]
        sel = band & (pred > pred[band].max() * 1e-4) if pred[band].max() > 0 else band
        excess = 10 * np.log10(np.maximum(c[band] - pred[band], 0).sum() / max(c[band].sum(), 1e-12) + 1e-9)
        lsd = float(np.average(np.abs(10 * np.log10(c[sel] / pred[sel])), weights=c[sel])) if sel.any() else 0.0
        hf = f >= 6000
        ph = (pred[hf] - noise[hf]).sum()
        hf_db = (10 * np.log10(max((c[hf] - noise[hf]).clip(0).sum(), 1e-12) / ph)
                 if ph > 0.1 * noise[hf].sum() else float("nan"))  # n/a: the render has no HF here
        clip = float(np.mean(np.abs(x) >= 32000)) * 100 if len(x) else 0.0
        run = np.flatnonzero((np.diff(x) == 0) & (np.abs(x[1:]) > 8000)) if len(x) > 1 else []
        ref_peak = 20 * np.log10(np.max(np.abs(ref)) / 32768 + 1e-9) if len(ref) else -200
        thd = ""
        sw = names[i]
        if sw.startswith("tone"):
            m = re.match(r"tone(\d+)(k?)", sw)
            f0 = float(m.group(1)) * (1000.0 if m.group(2) else 1.0)
            full, ff = psd(x, crate, 5.0)
            def pk(hz):
                k = int(round(hz / 5.0))
                return full[max(0, k - 3):k + 4].sum()
            h = sum(pk(f0 * n) for n in range(2, 9) if f0 * n < 20000)
            thd = "THD %5.1f dB" % (10 * np.log10(h / max(pk(f0), 1e-12) + 1e-12))
        score = excess + lsd / 3
        rows.append((name, excess, lsd, hf_db, clip, len(run), ref_peak, thd, faults_str(w["f0"]),
                     faults_str(w["f1"]), score))
        with wave.open("%s_%02d_%s_cap.wav" % (stem, i, name), "wb") as o:
            o.setnchannels(1), o.setsampwidth(2), o.setframerate(crate)
            o.writeframes(np.clip(x, -32768, 32767).astype("<i2").tobytes())
    print("%-15s %8s %6s %7s %6s %5s %8s %-13s %-22s %s" % ("window", "excess", "LSD", "HF>6k", "clip%",
                                                          "flat", "refpeak", "tone", "amp 0x4d faults",
                                                          "0x4e faults"))
    for r in rows:
        print("%-15s %6.1fdB %4.1fdB %5.1fdB %6.2f %5d %6.1fdB %-13s %-22s %s" % r[:10])
    sfx = sorted((r for r in rows if not r[0].startswith(("tone", "silence", "music"))), key=lambda r: -r[10])
    print("worst effects (captured energy the PC render + speaker response does not explain):")
    for r in sfx[:4]:
        print("  %-15s excess %5.1f dB, LSD %4.1f dB, HF %+5.1f dB%s" % (
            r[0], r[1], r[2], r[3], ", amp faults " + r[8] + "/" + r[9] if r[8] != "-" or r[9] != "-" else ""))
    return rows


def fake_cap(kind):
    """Synthetic TRC2 captures for --selftest: all-zero, write storm, good."""
    rng = np.random.default_rng(1)
    names = ["silence", "tone1k_-12dBFS", "tone1k_-3dBFS", "tone200_-6dBFS", "music"]
    lens = [23808, 47616, 47616, 47616, 480000]
    win = []
    for n, name in zip(lens, names):
        if kind == "zero":
            x = np.zeros(n)
        elif name.startswith("tone1k"):
            x = 2000 * np.sin(2 * np.pi * 1000 * np.arange(n) / 48000) + rng.normal(0, 30, n)
        elif name == "music":
            x = rng.normal(0, 500, n)
        else:
            x = rng.normal(0, 30, n)
        win.append({"pcm": x, "frames": n, "f0": 0, "f1": 0,
                    "type": 0 if name == "silence" else 2 if name == "music" else 1})
    want = (sum(lens) // 768 + 4) * 16  # real-time ms of those windows + the pre-roll
    return {"rate": 48000, "frames": sum(lens), "wfail": 1455 if kind != "good" else 0, "mfail": 0,
            "played": 187722 if kind != "good" else want + 20, "want": want, "win": win}


def selftest():
    for kind, want in (("zero", False), ("storm", False), ("good", True)):
        why = verdict(cap_stats(fake_cap(kind)))
        assert (why is None) == want, (kind, why)
        print("selftest verdict %-6s -> %s" % (kind, "PASS" if why is None else "FAIL: " + why))
    short = fake_cap("good")
    short["frames"] -= 768  # one mic block missing
    assert verdict(cap_stats(short)) == "capture incomplete"
    print("selftest verdict short  -> FAIL: capture incomplete")
    st = cap_stats(fake_cap("good"))
    st["wfail"] = 0
    st["w"] = [(0.0, 0.0)] + st["w"][1:]
    assert verdict(st) is not None  # digitally silent mic never passes
    # compare(): render the real script, fake a capture (48 kHz, a speaker-ish
    # low-pass, room noise), hard-clip ONE effect -- it must rank worst.
    import tempfile
    stem = os.path.join(tempfile.mkdtemp(), "st")
    cap0 = {"ver": 3, "synth_rate": 16000}
    names, refs, rrate, keys = reference(cap0, stem)
    win = []
    for name, r, key in zip(names, refs, keys):
        t = np.arange(len(r) * 3) / 48000.0
        up = np.interp(t, np.arange(len(r)) / rrate, r)
        up = np.convolve(up, np.ones(4) / 4, "same") * 0.05
        if name == "jump":
            up = np.clip(up * 8, -60, 60)  # gross distortion on one clip
        win.append({"pcm": up + rng_noise(len(up)), "frames": len(up), "f0": 0, "f1": 0, "cyc": 0,
                    "type": key[0], "kind": key[1], "param": key[2], "hz": key[3]})
    cap = {"rate": 48000, "win": win, "ver": 3, "synth_rate": 16000}
    rows = compare(cap, stem)
    worst = sorted((r for r in rows if not r[0].startswith(("tone", "silence", "music"))), key=lambda r: -r[10])
    assert worst[0][0] == "jump", worst[0]
    print("selftest compare: clipped 'jump' ranked worst")
    print("selftest ok")


def rng_noise(n, _r=np.random.default_rng(7)):
    return _r.normal(0, 3, n)


def read_wav(path):
    with wave.open(path, "rb") as w:
        assert w.getsampwidth() == 2, "16-bit PCM only"
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
        return x[::w.getnchannels()], w.getframerate()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inputs", nargs="*")
    ap.add_argument("--capture", help="raw capture dump (header + S16)")
    ap.add_argument("-o", help="output stem for --capture")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        selftest()
        return
    ok = True
    if a.capture:
        cap = parse(open(a.capture, "rb").read())
        if cap is None:
            sys.exit("RESULT FAIL: not a complete TRC2 sound-test capture (run unfinished, or play-only image)")
        stem = a.o or os.path.splitext(a.capture)[0]
        print("synth %d Hz V%d, volume %d, amp overrides %d; played %d ms of %d, %d write failures, "
              "%d mic failures (%d restarts)" % (cap["synth_rate"], cap["ver"], cap["volume"],
                                                 cap["overrides"], cap["played"], cap["want"], cap["wfail"],
                                                 cap["mfail"], cap["restarts"]))
        why = verdict(cap_stats(cap))
        compare(cap, stem)
        allpcm = np.concatenate([w["pcm"] for w in cap["win"]])
        spectro(allpcm, cap["rate"], stem + ".png")
        print("RESULT PASS: the mic heard the tone and the music" if why is None else "RESULT FAIL: " + why)
        ok = why is None
    for p in a.inputs:
        x, rate = read_wav(p)
        spectro(x, rate, os.path.splitext(p)[0] + ".png")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
