#!/usr/bin/env python3
"""tools/npu_probe_payload.py -- the MRAM payload for probe/npu (layout: probe/npu/src/payload.h).

    .venv/bin/python tools/npu_probe_payload.py --cut /tmp/tr-npu-models/movenet_cut.tflite \\
        --frames /tmp/tr-npu-frames --out /tmp/tr-npu-probe/payload.bin \\
        --vela-config <Alif sdk-alif>/samples/modules/executorch/ensemble_vela.ini

Frames are the ones tools/npu_body_proto.py --vectors saved (frame_<name>.npy)
plus an empty-booth frame. Expected results are the host's: LiteRT on the cut
(pre-Vela) model, decoded by src/vision/movenet.c. An NPU is not bit-exact to
the TFLite reference kernels, so the probe reports map CRC matches as
information and judges on the decoded pose.
"""
import argparse
import os
import struct
import subprocess
import sys
import zlib

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import npu_body_proto as proto  # noqa: E402

MAGIC, VERSION = 0x504E5254, 1
HDR = struct.Struct("<9I4II16s")  # tr_npu_payload_t, 72 B


def align(n, a=16):
    return (n + a - 1) // a * a


def pose_box_valid(kp):
    """tr_pose_box()'s validity rule (src/vision/pose.c): a confident shoulder and hip."""
    ok = [s >= 77 for _, _, s in kp]
    return (ok[5] or ok[6]) and (ok[11] or ok[12])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cut", required=True)
    ap.add_argument("--frames", default="/tmp/tr-npu-frames")
    ap.add_argument("--out", required=True)
    ap.add_argument("--vela", default=os.path.join(os.path.dirname(sys.executable), "vela"))
    ap.add_argument("--vela-config", required=True, help="Alif ensemble_vela.ini (not redistributed)")
    ap.add_argument("--accel", default="ethos-u55-256")
    ap.add_argument("--system-config", default="RTSS_HP_SRAM_MRAM")
    ap.add_argument("--memory-mode", default="Shared_Sram")
    a = ap.parse_args()

    od = os.path.dirname(os.path.abspath(a.out))
    vd = os.path.join(od, "vela")
    subprocess.run([a.vela, a.cut, "--accelerator-config", a.accel, "--config", a.vela_config,
                    "--system-config", a.system_config, "--memory-mode", a.memory_mode, "--optimise", "Size",
                    "--output-dir", vd], check=True, stdout=subprocess.DEVNULL)
    model = open(os.path.join(vd, os.path.splitext(os.path.basename(a.cut))[0] + "_vela.tflite"), "rb").read()

    rng = np.random.default_rng(3)
    empty = np.clip(proto.background() + rng.normal(0, 3, (proto.H, proto.W)), 0, 255).astype(np.uint8)
    frames = [("empty", empty)] + [(n, np.load(os.path.join(a.frames, f"frame_{n}.npy"))) for n in ("stand", "crouch")]

    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    dec = proto.Decoder(repo)
    cut = proto.interp(a.cut)
    expect = b""
    for name, img in frames:
        maps = proto.run_movenet_cut(cut, dec.input(img))
        kp = dec(maps)
        expect += struct.pack("<4I", *(zlib.crc32(m.tobytes()) for m in maps))
        expect += b"".join(struct.pack("<hhBB", x, y, s, 0) for x, y, s in kp)
        expect += struct.pack("<Bx", int(pose_box_valid(kp)))
        print(f"{name:7s} person={int(pose_box_valid(kp))} crc="
              + " ".join(f"{zlib.crc32(m.tobytes()):08x}" for m in maps))

    off = align(HDR.size)
    expect_off, off = off, align(off + len(expect))
    model_off, off = off, align(off + len(model))
    frame_off = []
    for _, img in frames:
        frame_off.append(off)
        off = align(off + img.size)
    total = off
    hdr = HDR.pack(MAGIC, VERSION, total, model_off, len(model), zlib.crc32(model), len(frames), proto.W,
                   proto.H, *(frame_off + [0] * (4 - len(frame_off))), expect_off, a.accel.encode())
    blob = bytearray(total)
    blob[0:HDR.size] = hdr
    blob[expect_off:expect_off + len(expect)] = expect
    blob[model_off:model_off + len(model)] = model
    for fo, (_, img) in zip(frame_off, frames):
        blob[fo:fo + img.size] = img.tobytes()
    open(a.out, "wb").write(blob)
    end = 0x80100000 + total
    print(f"{a.out}: {total} B, MRAM 0x80100000..0x{end - 1:08X}; model {len(model)} B crc {zlib.crc32(model):08x}")
    assert end <= 0x80558000, "payload runs into the ATOC package"


if __name__ == "__main__":
    main()
