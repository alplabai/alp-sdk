#!/usr/bin/env python3
"""tools/npu_body_proto.py -- host prototype for NPU body control (phase 1).

Builds a synthetic 640x400 GREY8 "OV9281" clip of a real person moving
left / right / jumping / crouching / leaving, runs the candidate int8 models on
it with the TFLite reference interpreter (LiteRT), and writes the MoveNet
keypoint trace as a C header the host tests replay through src/vision.

    python3 -m venv .venv && .venv/bin/pip install ai-edge-litert ethos-u-vela numpy pillow
    .venv/bin/python tools/npu_body_proto.py --models /tmp/tr-npu-models --work /tmp/tr-npu-frames \\
        --header tests/host/tr_pose_clip.h --vectors tests/host/data

The clip trace is the FIRMWARE path: the cut model (tools/movenet_cut.py,
movenet_cut.tflite in --models) on LiteRT, decoded by src/vision/movenet.c
(built here as a shared library). --vectors dumps two frames' raw cut-model
outputs plus the FULL published model's keypoints, which test_movenet.c
checks the C decoder against.

Sources (downloaded into --work/src on first run, never committed):
  stand  -- "Man holding top hat, full-length portrait, standing, facing front",
            Library of Congress LCCN2005681267 via Wikimedia Commons, public domain.
  crouch -- "Convair employee squatting next to rectangular panel", San Diego Air
            and Space Museum archive 23_0073712 via Wikimedia Commons, public domain
            (no known copyright restrictions).
Models (--models, see docs/superpowers/specs/2026-09-24-npu-body-control-design.md):
  movenet_lightning_int8.tflite  Google MoveNet SinglePose Lightning int8 v4, Apache-2.0
  yolo_face.tflite               yolo-fastest_192_face_v4 (emza-vs ModelZoo), Apache-2.0
  ssd_mnv1_coco_uint8_2018.tflite  TF coco_ssd_mobilenet_v1_1.0_quant_2018_06_29, Apache-2.0
"""
import argparse
import ctypes
import os
import subprocess
import urllib.request

import numpy as np
from PIL import Image

W, H = 640, 400  # OV9281 GREY8 mode (camera.c)
N_IN = 192       # MoveNet / yolo-fastest input
FEET_Y = 385     # where the player's feet are in the frame
STAND_H = 300    # standing player height in frame px (~3 m from a 70 deg HFOV lens)

SRC = {
    "stand.jpg": "https://upload.wikimedia.org/wikipedia/commons/d/de/"
                 "Man_holding_top_hat%2C_full-length_portrait%2C_standing%2C_facing_front_LCCN2005681267.jpg",
    "crouch.jpg": "https://upload.wikimedia.org/wikipedia/commons/c/cf/"
                  "23_0073712_Convair_Negative_Image_-_Convair_employee_squatting_next_to_rectangular_panel_"
                  "%2854267303017%29.jpg",
}
# Person crops in source pixels, and the source rows of the head top and the
# feet (from a MoveNet pass over the full photo; see the design doc).
CROP = {"stand.jpg": (1000, 880, 2150, 3420), "crouch.jpg": (150, 110, 830, 820)}
HEAD_FEET = {"stand.jpg": (880, 3300), "crouch.jpg": None}
SHOULDER_W = {"stand.jpg": 504.0, "crouch.jpg": 282.0}  # |lsho.x - rsho.x| from that pass


def fetch(work):
    os.makedirs(os.path.join(work, "src"), exist_ok=True)
    for name, url in SRC.items():
        p = os.path.join(work, "src", name)
        if not os.path.exists(p):
            req = urllib.request.Request(url, headers={"User-Agent": "trace-runner-npu-proto/0.1 (contact@alplab.ai)"})
            with urllib.request.urlopen(req) as r, open(p, "wb") as f:
                f.write(r.read())


def sprites(work):
    out = {}
    s_stand = STAND_H / (HEAD_FEET["stand.jpg"][1] - HEAD_FEET["stand.jpg"][0])
    for name in SRC:
        im = Image.open(os.path.join(work, "src", name)).convert("L").crop(CROP[name])
        # Same body scale for both people: match shoulder widths.
        s = s_stand * SHOULDER_W["stand.jpg"] / SHOULDER_W[name]
        im = im.resize((max(1, int(im.width * s)), max(1, int(im.height * s))), Image.BILINEAR)
        a = np.asarray(im, np.float32)
        # Feathered alpha so the crop rectangle is not a hard edge the model can latch onto.
        yy, xx = np.mgrid[0:a.shape[0], 0:a.shape[1]]
        d = np.minimum.reduce([xx, yy, a.shape[1] - 1 - xx, a.shape[0] - 1 - yy]).astype(np.float32)
        alpha = np.clip(d / 12.0, 0, 1)
        out[name] = (a, alpha)
    return out


def background():
    y, x = np.mgrid[0:H, 0:W]
    bg = 90 + 40 * (y / H) + 10 * np.sin(x / 37.0)
    bg[300:, :] = (70 + 20 * ((x // 40) % 2))[300:]  # a floor
    bg[60:200, 20:120] = 150                 # a booth poster
    bg[40:260, 520:600] = 55                 # a dark banner
    return bg


def script():
    """Per-frame (pose, centre_x, lift_px, visible) for the 30 Hz clip, plus labels."""
    L, C, R = 107, 320, 533  # the three lane centres at 640 wide (thirds)
    f = []

    def seg(n, pose, x0, x1, lift=0, vis=True):
        for i in range(n):
            t = (i + 1) / n
            f.append((pose, x0 + (x1 - x0) * t, lift, vis))

    seg(10, "stand.jpg", 0, 0, vis=False)        # empty booth
    seg(10, "stand.jpg", 700, C)                  # walks in
    seg(40, "stand.jpg", C, C)                    # stands still: calibration
    seg(8, "stand.jpg", C, L)                     # steps left
    seg(22, "stand.jpg", L, L)
    seg(16, "stand.jpg", L, R)                    # steps right, two lanes
    seg(24, "stand.jpg", R, R)
    seg(8, "stand.jpg", R, C)                     # back to centre
    seg(22, "stand.jpg", C, C)
    for i in range(14):                           # jump: 0.47 s airborne, 70 px peak
        t = (i + 1) / 15
        f.append(("stand.jpg", C, int(4 * 70 * t * (1 - t)), True))
    seg(24, "stand.jpg", C, C)
    seg(30, "crouch.jpg", C, C)                   # crouch 1 s
    seg(24, "stand.jpg", C, C)
    seg(12, "stand.jpg", C, -200)                 # walks off
    seg(40, "stand.jpg", 0, 0, vis=False)
    return f


def render(spr, bg, fr, rng):
    pose, cx, lift, vis = fr
    img = bg.copy()
    if vis:
        a, alpha = spr[pose]
        h, w = a.shape
        x0, y0 = int(cx - w / 2), int(FEET_Y - lift - h)
        xs, ys = max(0, x0), max(0, y0)
        xe, ye = min(W, x0 + w), min(H, y0 + h)
        if xe > xs and ye > ys:
            sa = a[ys - y0:ye - y0, xs - x0:xe - x0]
            sal = alpha[ys - y0:ye - y0, xs - x0:xe - x0]
            img[ys:ye, xs:xe] = img[ys:ye, xs:xe] * (1 - sal) + sa * sal
    img = img + rng.normal(0, 3, img.shape)  # sensor noise
    return np.clip(img, 0, 255).astype(np.uint8)


def letterbox_geom():
    """tr_movenet_input()'s letterbox: scale 192/640 = 0.3, 120 image rows, 36 rows of pad."""
    s = N_IN / W
    return s, (N_IN - int(H * s)) // 2


def interp(path):
    from ai_edge_litert.interpreter import Interpreter
    it = Interpreter(model_path=path)
    it.allocate_tensors()
    return it


def run_movenet_ref(it, x, s, pad):
    """The published model, whole, on the same pixels as the cut model (its uint8 input is the
    int8 tensor + 128): 17 x (y, x, score) out."""
    i, o = it.get_input_details()[0], it.get_output_details()[0]
    it.set_tensor(i["index"], (x.astype(np.int16) + 128).astype(np.uint8).reshape(1, N_IN, N_IN, 3))
    it.invoke()
    k = it.get_tensor(o["index"])[0, 0]  # normalised to the 192 square
    return [(int(round((x * N_IN) / s)), int(round((y * N_IN - pad) / s)), int(round(sc * 255))) for y, x, sc in k]


def run_movenet_cut(it, x):
    """The firmware's model: the int8 [192][192][3] tensor tr_movenet_input() made, in; the four
    raw int8 maps out, in the C decoder's order (centre, heat, offset, regress)."""
    it.set_tensor(it.get_input_details()[0]["index"], x.reshape(1, N_IN, N_IN, 3))
    it.invoke()
    outs = {o["name"]: it.get_tensor(o["index"]).reshape(-1) for o in it.get_output_details()}
    pick = lambda pfx: next(v for n, v in outs.items() if n.startswith(pfx))
    return [np.ascontiguousarray(pick(p)) for p in ("Reshape_2", "Sigmoid", "kpt_offset", "kpt_regress")]


def float_decode(maps, s, pad):
    """MoveNet's own tail, in float numpy, on the cut model's maps -- the exact algorithm
    src/vision/movenet.c approximates in integers (the published model's tail runs it
    quantised, so it disagrees with both by up to a cell on ties)."""
    ci = int(np.argmax(maps[0]))
    cy, cx = divmod(ci, 48)
    heat = (maps[1].reshape(48, 48, 17).astype(np.float64) + 128) / 256
    off = (maps[2].reshape(48, 48, 34).astype(np.float64) + 9) * 0.19978105
    reg = (maps[3].reshape(48, 48, 34).astype(np.float64) + 21) * 0.68843985
    yy, xx = np.mgrid[0:48, 0:48]
    out = []
    for k in range(17):
        ry, rx = cy + reg[cy, cx, 2 * k], cx + reg[cy, cx, 2 * k + 1]
        ky, kx = divmod(int(np.argmax(heat[:, :, k] / (np.sqrt((yy - ry) ** 2 + (xx - rx) ** 2) + 1.8))), 48)
        y, x = (ky + off[ky, kx, 2 * k]) * 4, (kx + off[ky, kx, 2 * k + 1]) * 4
        out.append((int(round(x / s)), int(round((y - pad) / s)), min(255, int(round(heat[ky, kx, k] * 256)))))
    return out


class Decoder:
    """src/vision/movenet.c, compiled for the host and called through ctypes."""

    def __init__(self, repo):
        so = "/tmp/tr-npu-movenet.so"
        subprocess.run(["cc", "-std=c11", "-O2", "-shared", "-fPIC", "-o", so,
                        os.path.join(repo, "src/vision/movenet.c")], check=True)
        self.lib = ctypes.CDLL(so)

    def input(self, frame):
        x = np.zeros(N_IN * N_IN * 3, np.int8)
        g = np.ascontiguousarray(frame, np.uint8)
        self.lib.tr_movenet_input(g.ctypes.data_as(ctypes.c_void_p), ctypes.c_int16(W), ctypes.c_int16(H),
                                  x.ctypes.data_as(ctypes.c_void_p))
        return x

    def __call__(self, maps):
        ptrs = [m.ctypes.data_as(ctypes.c_void_p) for m in maps]
        out = (ctypes.c_uint8 * (17 * 6))()  # tr_pose_t: 17 x {int16 x, int16 y, uint8 score} + pad = 6 B each
        self.lib.tr_movenet_decode((ctypes.c_void_p * 4)(*ptrs), ctypes.c_int16(W), ctypes.c_int16(H), out)
        b = bytes(out)
        return [(int.from_bytes(b[6 * k:6 * k + 2], "little", signed=True),
                 int.from_bytes(b[6 * k + 2:6 * k + 4], "little", signed=True), b[6 * k + 4]) for k in range(17)]


# yolo-fastest_192_face_v4 anchors (Arm ML embedded evaluation kit, object_detection use case).
FACE_ANCHORS = {6: [(38, 77), (47, 97), (61, 126)], 12: [(14, 26), (19, 37), (28, 55)]}


def run_face(it, sq, s, pad):
    i = it.get_input_details()[0]
    sc, zp = i["quantization"]
    it.set_tensor(i["index"], np.clip(np.round(sq / 255.0 / sc + zp), -128, 127).astype(np.int8)[None, :, :, None])
    it.invoke()
    best = None
    for o in it.get_output_details():
        t = it.get_tensor(o["index"])[0].astype(np.float32)
        osc, ozp = o["quantization"]
        t = (t - ozp) * osc
        g = t.shape[0]
        t = t.reshape(g, g, 3, 6)
        sig = 1 / (1 + np.exp(-t))
        for gy in range(g):
            for gx in range(g):
                for a in range(3):
                    conf = sig[gy, gx, a, 4] * sig[gy, gx, a, 5]
                    if best is None or conf > best[0]:
                        bx = (gx + sig[gy, gx, a, 0]) / g * N_IN
                        by = (gy + sig[gy, gx, a, 1]) / g * N_IN
                        bw = np.exp(t[gy, gx, a, 2]) * FACE_ANCHORS[g][a][0]
                        bh = np.exp(t[gy, gx, a, 3]) * FACE_ANCHORS[g][a][1]
                        best = (conf, bx / s, (by - pad) / s, bw / s, bh / s)
    return best


def run_ssd(it, frame):
    i = it.get_input_details()[0]
    x = np.asarray(Image.fromarray(frame).resize((300, 300), Image.BILINEAR))
    it.set_tensor(i["index"], np.repeat(x[None, :, :, None], 3, -1).astype(np.uint8))
    it.invoke()
    o = it.get_output_details()
    boxes, cls, score = (it.get_tensor(o[k]["index"])[0] for k in range(3))
    best = None
    for b, c, sc in zip(boxes, cls, score):
        if int(c) == 0 and (best is None or sc > best[0]):  # COCO class 0 = person
            best = (float(sc), b[1] * W, b[0] * H, (b[3] - b[1]) * W, (b[2] - b[0]) * H)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", default="/tmp/tr-npu-models")
    ap.add_argument("--work", default="/tmp/tr-npu-frames")
    ap.add_argument("--header", default=None, help="write the MoveNet trace as a C header")
    ap.add_argument("--vectors", default=None, help="dump decoder test vectors into this dir")
    ap.add_argument("--save-frames", action="store_true")
    a = ap.parse_args()

    fetch(a.work)
    rng = np.random.default_rng(1)
    spr, bg, frames = sprites(a.work), background(), script()
    mv = interp(os.path.join(a.models, "movenet_lightning_int8.tflite"))
    cut = interp(os.path.join(a.models, "movenet_cut.tflite"))
    dec = Decoder(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    vec_frames = {40: "stand", 210: "crouch"}
    refs = {}
    fc = interp(os.path.join(a.models, "yolo_face.tflite"))
    ssd = interp(os.path.join(a.models, "ssd_mnv1_coco_uint8_2018.tflite"))

    trace, rows = [], []
    for n, fr in enumerate(frames):
        img = render(spr, bg, fr, rng)
        if a.save_frames:
            Image.fromarray(img).save(os.path.join(a.work, f"f{n:03d}.png"))
        s, pad = letterbox_geom()
        x = dec.input(img)
        sq = (x.reshape(N_IN, N_IN, 3)[:, :, 0].astype(np.int16) + 128).astype(np.uint8)
        ref = run_movenet_ref(mv, x, s, pad)
        maps = run_movenet_cut(cut, x)
        kp = dec(maps)
        err = max(abs(a[0] - b[0]) + abs(a[1] - b[1]) for a, b in zip(kp, ref) if b[2] >= 77) if any(
            b[2] >= 77 for b in ref) else 0
        if a.vectors and n in vec_frames:
            os.makedirs(a.vectors, exist_ok=True)
            with open(os.path.join(a.vectors, f"movenet_{vec_frames[n]}.bin"), "wb") as f:
                for m in maps:
                    f.write(m.tobytes())
            refs[vec_frames[n]] = (ref, float_decode(maps, s, pad))
            np.save(os.path.join(a.work, f"frame_{vec_frames[n]}.npy"), img)
        face = run_face(fc, sq, s, pad)
        per = run_ssd(ssd, img)
        trace.append(kp)
        torso = np.mean([kp[k][2] for k in (5, 6, 11, 12)])
        rows.append((n, fr[3], fr[1], fr[2], fr[0][:-4], torso, face, per, err))

    print("frame vis  cx  lift pose   movenet_torso  face(conf,cx,top,h)        ssd_person(conf,cx,top,h)   "
          "C-decode vs full-model max |dx|+|dy| (px, confident kps)")
    for n, vis, cx, lift, pose, torso, face, per, err in rows:
        fs = f"{face[0]:.2f} {face[1] + face[3] / 2 * 0:6.0f} {face[2] - face[4] / 2:5.0f} {face[4]:4.0f}"
        ps = f"{per[0]:.2f} {per[1] + per[3] / 2:6.0f} {per[2]:5.0f} {per[4]:4.0f}" if per else "-"
        print(f"{n:5d} {int(vis)} {cx:5.0f} {lift:4d} {pose:6s} {torso:6.0f}        {fs:26s} {ps:26s} {err}")

    if a.vectors:
        with open(os.path.join(a.vectors, "movenet_ref.h"), "w", encoding="utf-8") as f:
            f.write("/* clang-format off */\n/* tests/host/data/movenet_ref.h -- GENERATED by tools/npu_body_proto.py; do not edit.\n"
                    " * For the frames whose cut-model maps are movenet_<name>.bin, {x, y, score} in\n"
                    " * 640x400 frame px: _ref = the FULL published MoveNet Lightning int8 model (LiteRT);\n"
                    " * _float = MoveNet's tail in float on the cut maps (tools/npu_body_proto.py\n"
                    " * float_decode). */\n")
            for name, pair in refs.items():
                for tag, kps in zip(("ref", "float"), pair):
                    f.write(f"static const int16_t movenet_{tag}_{name}[17][3] = {{\n")
                    f.write("".join(f"\t{{ {x}, {y}, {sc} }},\n" for x, y, sc in kps))
                    f.write("};\n")

    if a.header:
        with open(a.header, "w", encoding="utf-8") as f:
            f.write("/* clang-format off */\n/* tests/host/tr_pose_clip.h -- GENERATED by tools/npu_body_proto.py; do not edit.\n"
                    " * MoveNet SinglePose Lightning int8 (Apache-2.0) cut to its NPU body (tools/movenet_cut.py),\n"
                    " * run by LiteRT and decoded by src/vision/movenet.c, on a synthetic 640x400 GREY8 clip\n"
                    " * built from two public-domain photographs (see the tool).\n"
                    " * Per frame: 17 keypoints {x, y, score} in 640x400 frame px, score 0..255. */\n")
            f.write("#ifndef TR_POSE_CLIP_H\n#define TR_POSE_CLIP_H\n\n#include \"../../src/vision/pose.h\"\n\n")
            f.write(f"#define TR_POSE_CLIP_FRAMES {len(trace)}\n\n")
            f.write("static const tr_pose_t tr_pose_clip[TR_POSE_CLIP_FRAMES] = {\n")
            for kp in trace:
                f.write("\t{ { " + ", ".join(f"{{ {x}, {y}, {sc} }}" for x, y, sc in kp) + " } },\n")
            f.write("};\n\n#endif /* TR_POSE_CLIP_H */\n")


if __name__ == "__main__":
    main()
